#include "capture.h"
#include "renjob_adapters.h"
#include "rj_prof.h"
#include "spu_decode.h"
#include "unit.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <xmmintrin.h>

int rj_fuzz(const std::vector<uint8_t>& image, const std::vector<std::string>& args);
int rj_opfuzz(const std::vector<uint8_t>& image, const std::vector<std::string>& args);
int rj_hbench(const std::vector<uint8_t>& image, const std::vector<std::string>& args, const std::vector<std::string>& caps);

namespace {

constexpr uint32_t kImageEa = 0x705680;
constexpr uint32_t kImageSize = 0x13580;
constexpr uint32_t kImageLs = 0x4000;
constexpr uint32_t kEntry = 0x14cf8;
constexpr uint32_t kCodeEnd = kImageLs + kImageSize;

std::vector<uint8_t> g_elf;
std::vector<uint8_t> g_image;

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

bool rj_loaded(const uint8_t* ls) { return std::memcmp(ls + kImageLs, g_image.data(), 0x30) == 0; }

std::string cap_dir() {
    const char* d = std::getenv("RJ_CAPDIR");
    return d ? std::string(d) : std::string(GOWA_ROOT) + "/out/spucap_rj";
}

std::string cap_path(const std::string& s) {
    if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos) return s;
    return cap_dir() + "/" + s + (s.find(".bin") == std::string::npos ? ".bin" : "");
}

std::vector<std::string> cap_list(const std::vector<std::string>& args, size_t from) {
    std::vector<std::string> v;
    for (size_t i = from; i < args.size(); i++) v.push_back(cap_path(args[i]));
    if (v.empty())
        for (int i = 0; i < 6; i++) {
            std::string p = cap_path("cap_" + std::to_string(i));
            if (std::ifstream(p).good()) v.push_back(p);
        }
    return v;
}

using NativeFn = bool (*)(uint8_t* ls, spu::Context& c);

std::map<uint32_t, NativeFn>& natives() {
    static std::map<uint32_t, NativeFn> m = [] {
        std::map<uint32_t, NativeFn> t;
        size_t n = 0;
        const renjob::Adapter* a = renjob::adapters(n);
        for (size_t i = 0; i < n; i++) t[a[i].lsa] = a[i].handler;
        return t;
    }();
    return m;
}

bool is_call(const spudec::Insn& in, uint32_t pc, uint32_t npc, const spu::Context& c) {
    return (spudec::is(in.name, "brsl") || spudec::is(in.name, "brasl") || spudec::is(in.name, "bisl")) && npc != pc + 4 && in.dst >= 0 && c.gpr[in.dst].u32[3] == pc + 4;
}

int cmd_replay(const std::vector<std::string>& args) {
    int bad = 0;
    for (const std::string& path : cap_list(args, 1)) {
        edgecap::Capture cap;
        if (!cap.load(path)) {
            std::printf("%s: bad capture\n", path.c_str());
            bad++;
            continue;
        }
        edgecap::Machine m(cap);
        uint64_t rj_insns = 0, entries = 0;
        bool loaded0 = rj_loaded(m.c.ls);
        while (!m.done()) {
            bool ld = rj_loaded(m.c.ls);
            if (m.c.pc == kEntry && ld) entries++;
            if (m.c.pc >= kImageLs && m.c.pc < kCodeEnd && ld) rj_insns++;
            m.step();
        }
        std::string diff;
        bool ok = m.verify_final(diff);
        std::printf("%s: insns %llu loaded-at-start %d job-entries %llu renjob-insns %llu (%.1f%%) %s%s\n", path.c_str(), (unsigned long long)m.n, loaded0, (unsigned long long)entries, (unsigned long long)rj_insns, 100.0 * rj_insns / std::max<uint64_t>(1, m.n), ok ? "exact" : "DIFF", diff.c_str());
        bad += !ok;
    }
    return bad ? 1 : 0;
}

struct FnStat {
    uint64_t calls = 0;
    uint64_t incl = 0;
    uint64_t excl = 0;
    uint64_t impure = 0;
    std::set<uint32_t> callers;
};

int cmd_calls(const std::vector<std::string>& args) {
    std::map<uint32_t, FnStat> stats;
    uint64_t total = 0, jobs = 0, outside = 0;
    std::map<std::string, uint64_t> dmas;
    std::map<uint32_t, uint64_t> chan_pc;
    for (const std::string& path : cap_list(args, 1)) {
        edgecap::Capture cap;
        if (!cap.load(path)) continue;
        edgecap::Machine m(cap);
        struct Frame {
            uint32_t fn;
            uint32_t ret;
            uint64_t start, child;
            bool impure;
        };
        std::vector<Frame> st;
        bool in_job = false;
        m.on_dma = [&](const edgecap::Dma& d, bool after) {
            if (!in_job || after) return;
            char b[128];
            std::snprintf(b, sizeof b, "cmd %02x pc %05x", d.cmd, d.pc);
            dmas[b]++;
        };
        while (!m.done()) {
            uint32_t pc = m.c.pc;
            if (!in_job && pc == kEntry && rj_loaded(m.c.ls)) {
                in_job = true;
                jobs++;
                st.clear();
                st.push_back({kEntry, m.c.gpr[0].u32[3], m.n, 0, false});
            }
            if (!in_job) {
                if (pc >= kImageLs && pc < kCodeEnd && rj_loaded(m.c.ls)) outside++;
                m.step();
                continue;
            }
            total++;
            uint32_t w = be32(m.c.ls + pc);
            spudec::Insn in = spudec::decode(w);
            size_t p0 = m.pos;
            m.step();
            if (m.pos != p0 && spudec::any_of(in.name, {"rdch", "wrch", "rchcnt", "stop", "stopd"})) {
                for (Frame& f : st) f.impure = true;
                chan_pc[pc]++;
            }
            uint32_t npc = m.c.pc;
            if (is_call(in, pc, npc, m.c)) {
                st.push_back({npc, pc + 4, m.n, 0, false});
                stats[npc].callers.insert(pc);
            } else {
                for (size_t k = st.size(); k-- > 0;) {
                    if (st[k].ret == npc && (k + 1 == st.size() || true)) {
                        while (st.size() > k) {
                            Frame f = st.back();
                            st.pop_back();
                            uint64_t incl = m.n - f.start;
                            FnStat& s = stats[f.fn];
                            s.calls++;
                            s.incl += incl;
                            s.excl += incl - f.child;
                            s.impure += f.impure;
                            if (!st.empty()) st.back().child += incl;
                        }
                        break;
                    }
                }
                if (st.empty()) in_job = false;
            }
        }
    }
    std::vector<std::pair<uint32_t, FnStat>> v(stats.begin(), stats.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.incl > b.second.incl; });
    std::printf("jobs %llu renjob insns %llu (%.0f per job), image insns outside job frames %llu\n", (unsigned long long)jobs, (unsigned long long)total, jobs ? double(total) / jobs : 0.0, (unsigned long long)outside);
    for (auto& [fn, s] : v) {
        std::printf("fn %05x calls %7llu incl %10llu (%5.1f%%) excl %10llu (%5.1f%%) per-call %8.1f impure %6llu callers", fn, (unsigned long long)s.calls, (unsigned long long)s.incl, 100.0 * s.incl / std::max<uint64_t>(1, total), (unsigned long long)s.excl,
                    100.0 * s.excl / std::max<uint64_t>(1, total), double(s.incl) / std::max<uint64_t>(1, s.calls), (unsigned long long)s.impure);
        int k = 0;
        for (uint32_t c : s.callers) {
            if (k++ > 7) break;
            std::printf(" %05x", c);
        }
        std::printf("\n");
    }
    for (auto& [k, n] : dmas) std::printf("dma %s x%llu\n", k.c_str(), (unsigned long long)n);
    for (auto& [pc, n] : chan_pc) std::printf("chan pc %05x x%llu\n", pc, (unsigned long long)n);
    return 0;
}

int cmd_hist(const std::vector<std::string>& args) {
    std::map<uint32_t, uint64_t> counts;
    uint64_t total = 0;
    for (const std::string& path : cap_list(args, 1)) {
        edgecap::Capture cap;
        if (!cap.load(path)) continue;
        edgecap::Machine m(cap);
        while (!m.done()) {
            uint32_t pc = m.c.pc;
            if (pc >= kImageLs && pc < kCodeEnd && rj_loaded(m.c.ls)) {
                counts[pc]++;
                total++;
            }
            m.step();
        }
    }
    for (auto& [pc, n] : counts) std::printf("%05x %llu\n", pc, (unsigned long long)n);
    std::fprintf(stderr, "total %llu\n", (unsigned long long)total);
    return 0;
}

void print_regs(const spu::Context& c, int lo, int hi, const char* indent) {
    for (int r = lo; r <= hi; r++) {
        const spu::v128& v = c.gpr[r];
        std::printf("%sr%-3d %08x %08x %08x %08x\n", indent, r, v.u32[3], v.u32[2], v.u32[1], v.u32[0]);
    }
}

int cmd_trace(const std::vector<std::string>& args) {
    if (args.size() < 3) return 2;
    edgecap::Capture cap;
    if (!cap.load(cap_path(args[1]))) return 2;
    int want = std::stoi(args[2]);
    int maxdepth = args.size() > 3 ? std::stoi(args[3]) : 3;
    int regs = args.size() > 4 ? std::stoi(args[4]) : 8;
    edgecap::Machine m(cap);
    int job = -1;
    bool in_job = false;
    std::vector<std::pair<uint32_t, uint32_t>> st;
    m.on_dma = [&](const edgecap::Dma& d, bool after) {
        if (!in_job || job != want || after) return;
        std::printf("%*sDMA cmd %02x pc %05x lsa %05x ea %08x size %x tag %u\n", int(st.size() * 2), "", d.cmd, d.pc, d.lsa, d.ea, d.size, d.tag);
    };
    m.on_write = [&](uint32_t ch, uint32_t v) {
        if (!in_job || job != want || (ch >= 16 && ch <= 21)) return;
        std::printf("%*swrch ch%u %08x pc %05x\n", int(st.size() * 2), "", ch, v, m.c.pc);
    };
    while (!m.done()) {
        uint32_t pc = m.c.pc;
        if (!in_job && pc == kEntry && rj_loaded(m.c.ls)) {
            in_job = true;
            job++;
            st.clear();
            st.push_back({kEntry, m.c.gpr[0].u32[3]});
            if (job == want) {
                std::printf("job %d at insn %llu\n", job, (unsigned long long)m.n);
                print_regs(m.c, 0, 1 + regs, "  ");
            }
        }
        if (!in_job) {
            m.step();
            continue;
        }
        uint32_t w = be32(m.c.ls + pc);
        spudec::Insn in = spudec::decode(w);
        uint64_t n0 = m.n;
        size_t p0 = m.pos;
        m.step();
        if (job == want && m.pos != p0 && spudec::any_of(in.name, {"rdch", "rchcnt"})) {
            std::printf("%*s%s ch%d -> %08x pc %05x\n", int(st.size() * 2), "", in.name, (w >> 7) & 0x7f, m.c.gpr[w & 0x7f].u32[3], pc);
        }
        uint32_t npc = m.c.pc;
        if (is_call(in, pc, npc, m.c)) {
            if (job == want && int(st.size()) <= maxdepth) {
                std::printf("%*scall %05x from %05x at %llu\n", int(st.size() * 2), "", npc, pc, (unsigned long long)n0);
                std::string ind(st.size() * 2 + 2, ' ');
                if (regs) print_regs(m.c, 3, 2 + regs, ind.c_str());
            }
            st.push_back({npc, pc + 4});
        } else {
            for (size_t k = st.size(); k-- > 0;) {
                if (st[k].second == npc) {
                    while (st.size() > k) {
                        if (job == want && int(st.size()) <= maxdepth + 1) std::printf("%*sret %05x r3 %08x %08x %08x %08x at %llu\n", int(st.size() * 2 - 2), "", st.back().first, m.c.gpr[3].u32[3], m.c.gpr[3].u32[2], m.c.gpr[3].u32[1], m.c.gpr[3].u32[0], (unsigned long long)m.n);
                        st.pop_back();
                    }
                    break;
                }
            }
            if (st.empty()) {
                in_job = false;
                if (job == want) return 0;
            }
        }
    }
    return 0;
}

void for_each_call(const std::vector<std::string>& caps, uint32_t fn, const edgeunit::HitFn& cb) {
    for (const std::string& path : caps) {
        edgecap::Capture cap;
        if (!cap.load(path)) continue;
        edgecap::Machine m(cap);
        int job = -1;
        bool active = false;
        edgeunit::Hit h;
        size_t pos0 = 0;
        uint64_t n0 = 0;
        while (!m.done()) {
            uint32_t pc = m.c.pc;
            if (pc == kEntry && rj_loaded(m.c.ls)) job++;
            if (!active && pc == fn && rj_loaded(m.c.ls)) {
                active = true;
                h = edgeunit::Hit{};
                h.cap = path;
                h.job = job;
                h.insn = m.n;
                h.fn = fn;
                h.ret = m.c.gpr[0].u32[3];
                h.sp = m.c.gpr[1].u32[3];
                h.before.ls.assign(m.ls.begin(), m.ls.begin() + spu::LS_SIZE);
                h.before.regs = m.c;
                pos0 = m.pos;
                n0 = m.n;
            }
            m.step();
            if (active && m.c.pc == h.ret && m.c.gpr[1].u32[3] == h.sp) {
                active = false;
                h.fn_insns = m.n - n0;
                h.pure = m.pos == pos0;
                h.after.ls.assign(m.ls.begin(), m.ls.begin() + spu::LS_SIZE);
                h.after.regs = m.c;
                if (!cb(h)) return;
            }
        }
    }
}

std::vector<uint32_t> parse_list(const std::string& list) {
    std::vector<uint32_t> v;
    size_t p = 0;
    while (p < list.size()) {
        size_t e = list.find(',', p);
        if (e == std::string::npos) e = list.size();
        v.push_back(std::stoul(list.substr(p, e - p), nullptr, 16));
        p = e + 1;
    }
    return v;
}

int regs_diff(const spu::Context& a, const spu::Context& b, bool skip_r0) {
    for (int i = skip_r0 ? 1 : 0; i < 128; i++)
        if (std::memcmp(&a.gpr[i], &b.gpr[i], 16) != 0) return i;
    return -1;
}

int unit_one(uint32_t fn, int maxhits, const std::vector<std::string>& caps) {
    bool verbose = std::getenv("RJ_VERBOSE") != nullptr;
    NativeFn nat = natives().count(fn) ? natives()[fn] : nullptr;
    if (!nat) {
        std::printf("no native for %05x\n", fn);
        return 1;
    }
    int hits = 0, nat_bad = 0, declined = 0, iso_fail = 0, region_exits = 0;
    uint64_t spu_insns = 0;
    double nat_sec = 0;
    std::map<uint32_t, int> decline_pcs, exit_pcs;
    for (const std::string& path : caps) {
        edgecap::Capture cap;
        if (!cap.load(path)) continue;
        edgecap::Machine m(cap);
        while (!m.done() && hits < maxhits) {
            if (m.c.pc == fn && rj_loaded(m.c.ls)) {
                hits++;
                edgeunit::Snapshot ns;
                ns.ls.assign(m.ls.begin(), m.ls.end());
                ns.regs = m.c;
                ns.regs.ls = ns.ls.data();
                auto t0 = std::chrono::steady_clock::now();
                bool ok = nat(ns.ls.data(), ns.regs);
                nat_sec += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                if (!ok) {
                    declined++;
                    decline_pcs[renjob::last_decline_pc()]++;
                    bool ls_same = std::memcmp(ns.ls.data(), m.ls.data(), spu::LS_SIZE) == 0;
                    int rd = regs_diff(ns.regs, m.c, false);
                    if (!ls_same || rd >= 0) {
                        nat_bad++;
                        std::printf("  decline left changes %s insn %llu: ls %s reg r%d\n", path.c_str(), (unsigned long long)m.n, ls_same ? "same" : "DIFF", rd);
                    }
                } else {
                    uint32_t exit_pc = ns.regs.gpr[0].u32[3] & spu::PC_MASK, exit_sp = ns.regs.gpr[1].u32[3];
                    bool region = exit_pc != (m.c.gpr[0].u32[3] & spu::PC_MASK);
                    region_exits += region;
                    exit_pcs[exit_pc]++;
                    edgeunit::Snapshot iso;
                    iso.ls.assign(m.ls.begin(), m.ls.begin() + spu::LS_SIZE);
                    iso.regs = m.c;
                    edgeunit::IsoResult r = edgeunit::run_isolated(iso, exit_pc, exit_sp);
                    if (!r.ok) {
                        iso_fail++;
                        nat_bad++;
                        if (iso_fail <= 5) std::printf("  native ran but SPU does not reach exit %05x purely (%s) %s insn %llu\n", exit_pc, r.error.c_str(), path.c_str(), (unsigned long long)m.n);
                    } else {
                        spu_insns += r.insns;
                        edgeunit::LsDiff d = edgeunit::diff_ls(ns.ls.data(), iso.ls.data(), 0, 0);
                        int rd = regs_diff(ns.regs, iso.regs, region);
                        if (d.bytes || rd >= 0) {
                            nat_bad++;
                            if (nat_bad <= 8 || verbose) std::printf("  native mismatch %s insn %llu exit %05x: ls_diff %zu [%05x..%05x] reg r%d\n", path.c_str(), (unsigned long long)m.n, exit_pc, d.bytes, d.first, d.last, rd);
                        }
                    }
                }
            }
            m.step();
        }
    }
    std::printf("fn %05x: hits %d ran %d (region exits %d) spu-insns %llu (%.0f/run) declined %d native-mismatch %d native-time %.3f ms (%.2f us/hit)\n", fn, hits, hits - declined, region_exits, (unsigned long long)spu_insns,
                hits - declined ? double(spu_insns) / (hits - declined) : 0.0, declined, nat_bad, nat_sec * 1e3, hits ? nat_sec * 1e6 / hits : 0.0);
    for (auto& [pc, k] : decline_pcs) std::printf("  declined at %05x: %d\n", pc, k);
    if (verbose)
        for (auto& [pc, k] : exit_pcs) std::printf("  exit %05x: %d\n", pc, k);
    return nat_bad ? 1 : 0;
}

int cmd_rtrace(const std::vector<std::string>& args) {
    if (args.size() < 4) return 2;
    uint32_t fn = std::stoul(args[1], nullptr, 16);
    int which = std::stoi(args[2]);
    edgecap::Capture cap;
    if (!cap.load(cap_path(args[3]))) return 2;
    edgecap::Machine m(cap);
    int k = 0;
    while (!m.done()) {
        if (m.c.pc == fn && rj_loaded(m.c.ls) && k++ == which) {
            edgeunit::Snapshot ns;
            ns.ls.assign(m.ls.begin(), m.ls.end());
            ns.regs = m.c;
            ns.regs.ls = ns.ls.data();
            bool ok = natives()[fn](ns.ls.data(), ns.regs);
            std::printf("native %s exit %05x sp %05x (entry sp %05x r0 %05x)\n", ok ? "ran" : "declined", ns.regs.gpr[0].u32[3], ns.regs.gpr[1].u32[3], m.c.gpr[1].u32[3], m.c.gpr[0].u32[3]);
            std::vector<uint32_t> last;
            for (int i = 0; i < 2000000 && !m.done(); i++) {
                uint32_t pc = m.c.pc;
                last.push_back(pc);
                size_t p0 = m.pos;
                m.step();
                if (m.pos != p0 || (ok && m.c.pc == (ns.regs.gpr[0].u32[3] & spu::PC_MASK) && m.c.gpr[1].u32[3] == ns.regs.gpr[1].u32[3])) {
                    std::printf("%s after %d insns at pc %05x; last pcs:", m.pos != p0 ? "channel event" : "exit reached", i + 1, pc);
                    for (size_t j = last.size() > 40 ? last.size() - 40 : 0; j < last.size(); j++) std::printf(" %05x", last[j]);
                    std::printf("\n");
                    return 0;
                }
            }
            return 0;
        }
        m.step();
    }
    return 0;
}

int cmd_rbench(const std::vector<std::string>& args) {
    if (args.size() < 4) return 2;
    uint32_t fn = std::stoul(args[1], nullptr, 16);
    int maxhits = std::stoi(args[2]);
    NativeFn nat = natives()[fn];
    if (!nat) return 2;
    double total = 0;
    int n = 0;
    for (const std::string& path : cap_list(args, 3)) {
        edgecap::Capture cap;
        if (!cap.load(path)) continue;
        edgecap::Machine m(cap);
        while (!m.done() && n < maxhits) {
            if (m.c.pc == fn && rj_loaded(m.c.ls)) {
                std::vector<uint8_t> ls(m.ls.begin(), m.ls.end());
                double best = 1e9;
                for (int k = 0; k < 20; k++) {
                    std::memcpy(ls.data(), m.ls.data(), spu::LS_SIZE);
                    spu::Context c = m.c;
                    c.ls = ls.data();
                    auto t0 = std::chrono::steady_clock::now();
                    nat(ls.data(), c);
                    best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
                }
                total += best;
                n++;
            }
            m.step();
        }
    }
    std::printf("fn %05x: %d hits, %.3f us/hit best (hot)\n", fn, n, n ? total * 1e6 / n : 0.0);
    return 0;
}

int cmd_unit(const std::vector<std::string>& args) {
    if (args.size() < 2) return 2;
    int maxhits = args.size() > 2 ? std::stoi(args[2]) : 1000000;
    std::vector<uint32_t> fns;
    if (args[1] == "all")
        for (auto& [k, v] : natives()) fns.push_back(k);
    else
        fns = parse_list(args[1]);
    int bad = 0;
    for (uint32_t fn : fns) bad += unit_one(fn, maxhits, cap_list(args, 3));
    return bad ? 1 : 0;
}

int cmd_itrace(const std::vector<std::string>& args) {
    if (args.size() < 4) return 2;
    uint32_t fn = std::stoul(args[1], nullptr, 16);
    int which = std::stoi(args[2]);
    std::string mode = args[3];
    int n = 0;
    class NullHostT : public spu::Host {
    public:
        uint32_t read_channel(uint32_t) override { return 0; }
        void write_channel(uint32_t, uint32_t) override {}
        uint32_t channel_count(uint32_t) override { return 0; }
        bool stop_and_signal(uint32_t) override { return false; }
        void halt(uint32_t, uint32_t) override {}
    };
    for_each_call(cap_list(args, 4), fn, [&](edgeunit::Hit& h) {
        if (n++ < which) return true;
        edgeunit::Snapshot s = h.before;
        s.ls.resize(spu::LS_SIZE + 16);
        s.regs.ls = s.ls.data();
        s.regs.line_epoch = nullptr;
        NullHostT host;
        std::map<uint32_t, uint64_t> counts;
        uint64_t k = 0;
        while (k < 50000000) {
            uint32_t pc = s.regs.pc;
            uint32_t w = be32(s.ls.data() + pc);
            spudec::Insn in = spudec::decode(w);
            uint32_t addr = 0;
            if (in.store || in.load) {
                if (in.pc_rel) addr = pc + in.imm * 4;
                else if (in.abs_addr) addr = in.imm * 4;
                else addr = s.regs.gpr[in.addr_a].u32[3] + (in.addr_b >= 0 ? s.regs.gpr[in.addr_b].u32[3] : uint32_t(in.imm));
                addr &= spu::LS_MASK;
            }
            spu::v128 sv = in.store ? s.regs.gpr[in.src[0]] : spu::v128{};
            spu::step(s.regs, host);
            k++;
            counts[pc]++;
            if (mode == "stores" && in.store) std::printf("%05x st %05x %08x %08x %08x %08x\n", pc, addr, sv.u32[3], sv.u32[2], sv.u32[1], sv.u32[0]);
            if (mode == "all") {
                std::printf("%05x %-28s", pc, spudec::disasm(w, pc).c_str());
                if (in.dst >= 0) {
                    const spu::v128& v = s.regs.gpr[in.dst];
                    std::printf(" r%d=%08x %08x %08x %08x", in.dst, v.u32[3], v.u32[2], v.u32[1], v.u32[0]);
                }
                if (in.store) std::printf(" [%05x]=%08x %08x %08x %08x", addr, sv.u32[3], sv.u32[2], sv.u32[1], sv.u32[0]);
                if (in.load) std::printf(" [%05x]", addr);
                std::printf("\n");
            }
            if (s.regs.pc == h.ret && s.regs.gpr[1].u32[3] == h.sp) break;
        }
        if (mode == "counts")
            for (auto& [pc, c] : counts) std::printf("%05x %llu\n", pc, (unsigned long long)c);
        std::printf("insns %llu\n", (unsigned long long)k);
        return false;
    });
    return 0;
}

std::map<uint32_t, uint64_t>* g_hist = nullptr;

struct PutRec {
    uint32_t cmd = 0, pc = 0, lsa = 0, ea = 0, size = 0;
    std::vector<uint8_t> data;
};

std::vector<PutRec> run_with_hooks(const std::string& path, const std::set<uint32_t>& hooks, uint64_t* hooked, uint64_t* skipped, uint64_t* declined, std::string* err, double* nat_sec) {
    std::vector<PutRec> puts;
    edgecap::Capture cap;
    if (!cap.load(path)) return puts;
    edgecap::Machine m(cap);
    m.on_dma = [&](const edgecap::Dma& d, bool after) {
        if (after) return;
        bool put = (d.cmd & 0xf0) == 0x20 || (d.cmd & 0xf0) == 0x30 || d.cmd == 0xb4 || d.cmd == 0xb0 || d.cmd == 0xb8;
        if (!put) return;
        PutRec p{d.cmd, d.pc, d.lsa, d.ea, d.size, {}};
        if (d.cmd >= 0xb0) p.data.assign(m.ls.begin() + (d.lsa & 0x3ff80), m.ls.begin() + (d.lsa & 0x3ff80) + 128);
        else if (d.cmd & 4) {
            uint32_t list = d.ea & 0x3fff8;
            uint32_t cur = d.lsa & 0x3fff0;
            for (uint32_t k = 0; k + 8 <= d.size; k += 8) {
                uint32_t sz = be32(m.ls.data() + ((list + k) & 0x3fff8)) & 0x7fff;
                uint32_t eea = be32(m.ls.data() + ((list + k + 4) & 0x3fffc));
                uint32_t lsa = cur | (eea & 0xf);
                for (uint32_t i = 0; i < sz; i++) p.data.push_back(m.ls[(lsa + i) & 0x3ffff]);
                cur = (cur + ((sz + 15) & ~15u)) & 0x3fff0;
            }
        } else if (d.size <= 0x4000)
            for (uint32_t i = 0; i < d.size; i++) p.data.push_back(m.ls[(d.lsa + i) & 0x3ffff]);
        puts.push_back(std::move(p));
    };
    while (!m.done()) {
        uint32_t pc = m.c.pc;
        if (!hooks.empty() && hooks.count(pc) && rj_loaded(m.c.ls)) {
            NativeFn nat = natives()[pc];
            edgeunit::Snapshot iso;
            iso.ls.assign(m.ls.begin(), m.ls.begin() + spu::LS_SIZE);
            iso.regs = m.c;
            std::vector<uint8_t> orig_ls = iso.ls;
            spu::Context nc = m.c;
            auto t0 = std::chrono::steady_clock::now();
            bool ok = nat(m.ls.data(), nc);
            if (nat_sec) *nat_sec += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (ok) {
                uint32_t exit_pc = nc.gpr[0].u32[3] & spu::PC_MASK, exit_sp = nc.gpr[1].u32[3];
                bool region = exit_pc != (m.c.gpr[0].u32[3] & spu::PC_MASK);
                edgeunit::IsoResult r = edgeunit::run_isolated(iso, exit_pc, exit_sp);
                if (r.ok && m.n + r.insns < cap.insns) {
                    if (err && err->empty()) {
                        int rd = regs_diff(nc, iso.regs, region);
                        if (rd >= 0) *err = "regs differ at hook " + std::to_string(pc) + " r" + std::to_string(rd);
                        else if (std::memcmp(iso.ls.data(), m.ls.data(), spu::LS_SIZE) != 0) *err = "ls differs at hook " + std::to_string(pc);
                    }
                    uint8_t* keep = m.c.ls;
                    m.c = nc;
                    m.c.ls = keep;
                    m.c.pc = exit_pc;
                    m.skip(r.insns);
                    if (hooked) (*hooked)++;
                    if (skipped) *skipped += r.insns;
                    continue;
                }
                if (!r.ok && err && err->empty()) *err = "native ran where SPU is impure at hook " + std::to_string(pc);
                std::memcpy(m.ls.data(), orig_ls.data(), spu::LS_SIZE);
            } else if (declined)
                (*declined)++;
        }
        if (g_hist && pc >= kImageLs && pc < kCodeEnd && rj_loaded(m.c.ls)) (*g_hist)[pc]++;
        m.step();
    }
    if (err && err->empty() && !m.error.empty()) *err = m.error;
    if (err && !hooks.empty()) {
        std::string diff;
        if (!m.verify_final(diff)) *err += " final-state:" + diff;
    }
    return puts;
}

int cmd_e2e(const std::vector<std::string>& args) {
    if (args.size() < 2) return 2;
    std::set<uint32_t> hooks;
    if (args[1] == "all") {
        for (auto& [k, v] : natives()) hooks.insert(k);
    } else
        for (uint32_t h : parse_list(args[1])) hooks.insert(h);
    if (const char* skip = std::getenv("RJ_SKIP"))
        for (uint32_t h : parse_list(skip)) hooks.erase(h);
    for (uint32_t h : hooks)
        if (!natives().count(h)) {
            std::printf("no native for %05x\n", h);
            return 2;
        }
    int bad = 0;
    std::map<uint32_t, uint64_t> hist;
    for (const std::string& path : cap_list(args, 2)) {
        g_hist = nullptr;
        std::vector<PutRec> ref = run_with_hooks(path, {}, nullptr, nullptr, nullptr, nullptr, nullptr);
        uint64_t hooked = 0, skipped = 0, declined = 0;
        std::string err;
        double nat_sec = 0;
        if (std::getenv("RJ_HIST")) g_hist = &hist;
        std::vector<PutRec> got = run_with_hooks(path, hooks, &hooked, &skipped, &declined, &err, &nat_sec);
        size_t mism = 0, rj_puts = 0;
        std::map<uint32_t, size_t> mism_pc;
        for (size_t i = 0; i < std::min(ref.size(), got.size()); i++) {
            const PutRec& a = ref[i];
            const PutRec& b = got[i];
            if (a.pc >= kImageLs && a.pc < kCodeEnd) rj_puts++;
            if (a.cmd != b.cmd || a.ea != b.ea || a.size != b.size || a.data != b.data) {
                mism++;
                mism_pc[a.pc]++;
                if (mism <= 5) {
                    size_t off = 0;
                    while (off < a.data.size() && off < b.data.size() && a.data[off] == b.data[off]) off++;
                    std::printf("  put %zu mismatch pc %05x cmd %02x ea %08x size %x vs ea %08x size %x first-diff-byte %zx\n", i, a.pc, a.cmd, a.ea, a.size, b.ea, b.size, off);
                }
            }
        }
        rjprof::dump();
        std::printf("%s: puts ref %zu got %zu (in-image pc %zu) mismatching %zu hooked %llu declined %llu skipped-insns %llu native %.1f ms %s\n", path.c_str(), ref.size(), got.size(), rj_puts, mism, (unsigned long long)hooked, (unsigned long long)declined, (unsigned long long)skipped, nat_sec * 1e3, err.c_str());
        for (auto& [pc, n] : mism_pc) std::printf("    mismatches at pc %05x: %zu\n", pc, n);
        if (mism || ref.size() != got.size() || !err.empty()) bad++;
    }
    if (const char* hf = std::getenv("RJ_HIST"))
        if (std::FILE* f = std::fopen(hf, "w")) {
            for (auto& [pc, n] : hist) std::fprintf(f, "%05x %llu\n", pc, (unsigned long long)n);
            std::fclose(f);
        }
    return bad ? 1 : 0;
}

int cmd_args(const std::vector<std::string>& args) {
    if (args.size() < 2) return 2;
    uint32_t fn = std::stoul(args[1], nullptr, 16);
    int maxhits = std::getenv("RJ_MAX") ? std::atoi(std::getenv("RJ_MAX")) : 1000000;
    int n = 0;
    for_each_call(cap_list(args, 2), fn, [&](edgeunit::Hit& h) {
        std::printf("%s job %d insns %llu pure %d", h.cap.substr(h.cap.size() - 9).c_str(), h.job, (unsigned long long)h.fn_insns, h.pure);
        for (int r = 3; r <= 10; r++) std::printf(" %05x", h.before.regs.gpr[r].u32[3]);
        std::printf(" r3out %08x\n", h.after.regs.gpr[3].u32[3]);
        return ++n < maxhits;
    });
    return 0;
}

int cmd_bench(const std::vector<std::string>& args) {
    if (args.size() < 2) return 2;
    uint32_t fn = std::stoul(args[1], nullptr, 16);
    int maxhits = args.size() > 2 ? std::stoi(args[2]) : 200;
    NativeFn nat = natives()[fn];
    if (!nat) return 2;
    double best_total = 0;
    uint64_t insns = 0;
    int n = 0;
    for_each_call(cap_list(args, 3), fn, [&](edgeunit::Hit& h) {
        double best = 1e9;
        for (int k = 0; k < 20; k++) {
            edgeunit::Snapshot ns = h.before;
            ns.ls.resize(spu::LS_SIZE + 16);
            ns.regs.ls = ns.ls.data();
            auto t0 = std::chrono::steady_clock::now();
            nat(ns.ls.data(), ns.regs);
            best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        }
        best_total += best;
        insns += h.fn_insns;
        return ++n < maxhits;
    });
    std::printf("fn %05x: %d hits, %.3f us/call best, %.2f ns per SPU insn\n", fn, n, n ? best_total * 1e6 / n : 0.0, insns ? best_total * 1e9 / insns : 0.0);
    return 0;
}

}

int main(int argc, char** argv) {
    _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x6000u | 0x8040u);
    std::string root = GOWA_ROOT;
    g_elf = read_file(root + "/bin/01.12/GOWA.patched.elf");
    if (g_elf.size() < kImageEa - 0x10000 + kImageSize) {
        std::fprintf(stderr, "cannot read elf\n");
        return 2;
    }
    g_image.assign(g_elf.begin() + (kImageEa - 0x10000), g_elf.begin() + (kImageEa - 0x10000) + kImageSize);
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string cmd = args.empty() ? "replay" : args[0];
    if (cmd == "replay") return cmd_replay(args);
    if (cmd == "calls") return cmd_calls(args);
    if (cmd == "hist") return cmd_hist(args);
    if (cmd == "trace") return cmd_trace(args);
    if (cmd == "unit") return cmd_unit(args);
    if (cmd == "rtrace") return cmd_rtrace(args);
    if (cmd == "rbench") return cmd_rbench(args);
    if (cmd == "itrace") return cmd_itrace(args);
    if (cmd == "e2e") return cmd_e2e(args);
    if (cmd == "args") return cmd_args(args);
    if (cmd == "bench") return cmd_bench(args);
    if (cmd == "fuzz") return rj_fuzz(g_image, args);
    if (cmd == "opfuzz") return rj_opfuzz(g_image, args);
    if (cmd == "hbench") return rj_hbench(g_image, args, cap_list(args, 3));
    std::fprintf(stderr, "unknown command\n");
    return 2;
}
