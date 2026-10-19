#include "capture.h"
#include "cs_prof.h"
#include "cloth/cloth_adapters.h"
#include "shadows/shadows_adapters.h"
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
#include <random>
#include <set>
#include <string>
#include <vector>

#include <xmmintrin.h>

int cs_fuzz(const std::string& job, const std::vector<uint8_t>& image, const std::vector<std::string>& args);
int cs_opfuzz(const std::string& job, const std::vector<uint8_t>& image, const std::vector<std::string>& args);
int cs_convfuzz(const std::vector<std::string>& args);
bool cs_has_gen(const std::string& job, uint32_t pc);
bool cs_gen_call(const std::string& job, uint32_t pc, uint8_t* ls, spu::Context& c);

namespace {

using NativeFn = bool (*)(uint8_t* ls, spu::Context& c);

struct JobDesc {
    const char* name;
    uint32_t ea;
    uint32_t size;
    uint32_t entry;
};

const JobDesc kJobs[] = {{"cloth", 0x728400, 0x83e0, 0xbf60}, {"shadows", 0x79c400, 0x9850, 0xcf70}};

const JobDesc* g_job = nullptr;
uint32_t kImageLs = 0x4000;
uint32_t g_code_end = 0;
std::vector<uint8_t> g_elf;
std::vector<uint8_t> g_image;

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

bool loaded(const uint8_t* ls) { return std::memcmp(ls + kImageLs, g_image.data(), 0x30) == 0; }

bool in_image(uint32_t pc) { return pc >= kImageLs && pc < g_code_end; }

const char* kSets[][2] = {{"base", "spucap"}, {"rj", "spucap_rj"}, {"rj2", "spucap_rj2"}, {"cs65", "spucap_cs65"}};

std::string set_dir(const std::string& s) {
    for (auto& k : kSets)
        if (s == k[0]) return std::string(GOWA_ROOT) + "/out/" + k[1];
    return {};
}

std::vector<std::string> expand_cap(const std::string& s) {
    std::vector<std::string> v;
    if (s.find('/') != std::string::npos && std::ifstream(s).good()) return {s};
    if (s == "all") {
        for (auto& k : kSets) {
            auto x = expand_cap(k[0]);
            v.insert(v.end(), x.begin(), x.end());
        }
        return v;
    }
    size_t c = s.find(':');
    std::string set = c == std::string::npos ? s : s.substr(0, c);
    std::string dir = set_dir(set);
    if (dir.empty()) return v;
    if (c != std::string::npos) return {dir + "/cap_" + s.substr(c + 1) + ".bin"};
    for (int i = 0; i < 6; i++) {
        std::string p = dir + "/cap_" + std::to_string(i) + ".bin";
        if (std::ifstream(p).good()) v.push_back(p);
    }
    return v;
}

std::vector<std::string> cap_list(const std::vector<std::string>& args, size_t from) {
    std::vector<std::string> v;
    for (size_t i = from; i < args.size(); i++) {
        auto x = expand_cap(args[i]);
        v.insert(v.end(), x.begin(), x.end());
    }
    if (from >= args.size()) {
        const char* d = std::getenv("CS_CAPS");
        std::string spec = d ? d : "all";
        size_t p = 0;
        while (p <= spec.size()) {
            size_t q = spec.find(',', p);
            if (q == std::string::npos) q = spec.size();
            auto x = expand_cap(spec.substr(p, q - p));
            v.insert(v.end(), x.begin(), x.end());
            p = q + 1;
        }
    }
    return v;
}

std::string short_name(const std::string& p) {
    size_t a = p.rfind("/out/");
    return a == std::string::npos ? p : p.substr(a + 5);
}

std::map<uint32_t, NativeFn>& natives() {
    static std::map<uint32_t, NativeFn> m = [] {
        std::map<uint32_t, NativeFn> t;
        size_t n = 0;
        const auto* ca = cloth::adapters(n);
        if (std::string(g_job->name) == "cloth")
            for (size_t i = 0; i < n; i++) t[ca[i].lsa] = ca[i].handler;
        const auto* sa = shadows::adapters(n);
        if (std::string(g_job->name) == "shadows")
            for (size_t i = 0; i < n; i++) t[sa[i].lsa] = sa[i].handler;
        return t;
    }();
    return m;
}

uint32_t last_decline() { return std::string(g_job->name) == "cloth" ? cloth::last_decline_pc() : shadows::last_decline_pc(); }

bool is_call(const spudec::Insn& in, uint32_t pc, uint32_t npc, const spu::Context& c) {
    return (spudec::is(in.name, "brsl") || spudec::is(in.name, "brasl") || spudec::is(in.name, "bisl")) && npc != pc + 4 && in.dst >= 0 && c.gpr[in.dst].u32[3] == pc + 4;
}

int cmd_replay(const std::vector<std::string>& args) {
    int bad = 0;
    uint64_t tj = 0, ti = 0, tall = 0;
    for (const std::string& path : cap_list(args, 1)) {
        edgecap::Capture cap;
        if (!cap.load(path)) {
            std::printf("%s: bad capture\n", path.c_str());
            bad++;
            continue;
        }
        edgecap::Machine m(cap);
        uint64_t ji = 0, entries = 0;
        bool loaded0 = loaded(m.c.ls);
        auto t0 = std::chrono::steady_clock::now();
        while (!m.done()) {
            uint32_t pc = m.c.pc;
            if (in_image(pc) && loaded(m.c.ls)) {
                ji++;
                if (pc == g_job->entry) entries++;
            }
            m.step();
        }
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::string diff;
        bool ok = m.verify_final(diff);
        std::printf("%-22s insns %9llu loaded-at-start %d entries %3llu job-insns %9llu (%5.1f%%) %.1fs %s%s\n", short_name(path).c_str(), (unsigned long long)m.n, loaded0, (unsigned long long)entries, (unsigned long long)ji, 100.0 * ji / std::max<uint64_t>(1, m.n), sec, ok ? "exact" : "DIFF", diff.c_str());
        bad += !ok;
        tj += entries;
        ti += ji;
        tall += m.n;
    }
    std::printf("total: entries %llu job-insns %llu of %llu (%.2f%%)\n", (unsigned long long)tj, (unsigned long long)ti, (unsigned long long)tall, 100.0 * ti / std::max<uint64_t>(1, tall));
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
            char b[160];
            std::snprintf(b, sizeof b, "cmd %02x pc %05x lsa %05x size %x", d.cmd, d.pc, d.lsa, d.size);
            dmas[b]++;
        };
        while (!m.done()) {
            uint32_t pc = m.c.pc;
            if (!in_job && pc == g_job->entry && loaded(m.c.ls)) {
                in_job = true;
                jobs++;
                st.clear();
                st.push_back({g_job->entry, m.c.gpr[0].u32[3], m.n, 0, false});
            }
            if (!in_job) {
                if (in_image(pc) && loaded(m.c.ls)) outside++;
                m.step();
                continue;
            }
            total++;
            uint32_t w = be32(m.c.ls + pc);
            spudec::Insn in = spudec::decode(w);
            size_t p0 = m.pos;
            m.step();
            if (m.pos != p0 && spudec::any_of(in.name, {"rdch", "wrch", "rchcnt", "stop", "stopd"})) {
                for (auto& f : st) f.impure = true;
                chan_pc[pc]++;
            }
            uint32_t npc = m.c.pc;
            if (is_call(in, pc, npc, m.c)) {
                st.push_back({npc, pc + 4, m.n, 0, false});
                stats[npc].callers.insert(pc);
            } else {
                for (size_t k = st.size(); k-- > 0;) {
                    if (st[k].ret == npc) {
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
    std::printf("jobs %llu job insns %llu (%.0f per job), image insns outside job frames %llu\n", (unsigned long long)jobs, (unsigned long long)total, jobs ? double(total) / jobs : 0.0, (unsigned long long)outside);
    for (auto& [fn, s] : v) {
        std::printf("fn %05x calls %7llu incl %10llu (%5.1f%%) excl %10llu (%5.1f%%) per-call %9.1f impure %6llu callers", fn, (unsigned long long)s.calls, (unsigned long long)s.incl, 100.0 * s.incl / std::max<uint64_t>(1, total), (unsigned long long)s.excl,
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
            if (in_image(pc) && loaded(m.c.ls)) {
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
    auto caps = expand_cap(args[1]);
    if (caps.empty()) return 2;
    edgecap::Capture cap;
    if (!cap.load(caps[0])) return 2;
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
        if (!in_job && pc == g_job->entry && loaded(m.c.ls)) {
            in_job = true;
            job++;
            st.clear();
            st.push_back({g_job->entry, m.c.gpr[0].u32[3]});
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
        if (job == want && m.pos != p0 && spudec::any_of(in.name, {"rdch", "rchcnt"})) std::printf("%*s%s ch%d -> %08x pc %05x\n", int(st.size() * 2), "", in.name, (w >> 7) & 0x7f, m.c.gpr[w & 0x7f].u32[3], pc);
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
            if (pc == g_job->entry && loaded(m.c.ls)) job++;
            if (!active && pc == fn && loaded(m.c.ls)) {
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
    bool verbose = std::getenv("CS_VERBOSE") != nullptr;
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
            if (m.c.pc == fn && loaded(m.c.ls)) {
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
                    decline_pcs[last_decline()]++;
                    bool ls_same = std::memcmp(ns.ls.data(), m.ls.data(), spu::LS_SIZE) == 0;
                    int rd = regs_diff(ns.regs, m.c, false);
                    if (!ls_same || rd >= 0) {
                        nat_bad++;
                        std::printf("  decline left changes %s insn %llu: ls %s reg r%d\n", short_name(path).c_str(), (unsigned long long)m.n, ls_same ? "same" : "DIFF", rd);
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
                        if (iso_fail <= 5) std::printf("  native ran but SPU does not reach exit %05x purely (%s) %s insn %llu\n", exit_pc, r.error.c_str(), short_name(path).c_str(), (unsigned long long)m.n);
                    } else {
                        spu_insns += r.insns;
                        edgeunit::LsDiff d = edgeunit::diff_ls(ns.ls.data(), iso.ls.data(), 0, 0);
                        int rd = regs_diff(ns.regs, iso.regs, region);
                        if (d.bytes || rd >= 0) {
                            nat_bad++;
                            if (nat_bad <= 8 || verbose) {
                                std::printf("  native mismatch %s insn %llu exit %05x: ls_diff %zu [%05x..%05x] reg r%d\n", short_name(path).c_str(), (unsigned long long)m.n, exit_pc, d.bytes, d.first, d.last, rd);
                                if (verbose && d.bytes) {
                                    int shown = 0;
                                    for (uint32_t a = d.first & ~15u; a <= d.last && shown < 12; a += 16)
                                        if (std::memcmp(ns.ls.data() + a, iso.ls.data() + a, 16)) {
                                            std::printf("    %05x nat %08x %08x %08x %08x spu %08x %08x %08x %08x\n", a, be32(&ns.ls[a]), be32(&ns.ls[a + 4]), be32(&ns.ls[a + 8]), be32(&ns.ls[a + 12]), be32(&iso.ls[a]), be32(&iso.ls[a + 4]), be32(&iso.ls[a + 8]), be32(&iso.ls[a + 12]));
                                            shown++;
                                        }
                                }
                                if (verbose && rd >= 0) {
                                    std::printf("    r%d nat %08x %08x %08x %08x spu %08x %08x %08x %08x\n", rd, ns.regs.gpr[rd].u32[3], ns.regs.gpr[rd].u32[2], ns.regs.gpr[rd].u32[1], ns.regs.gpr[rd].u32[0], iso.regs.gpr[rd].u32[3], iso.regs.gpr[rd].u32[2], iso.regs.gpr[rd].u32[1], iso.regs.gpr[rd].u32[0]);
                                }
                            }
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

int cmd_rtrace(const std::vector<std::string>& args) {
    if (args.size() < 4) return 2;
    uint32_t fn = std::stoul(args[1], nullptr, 16);
    int which = std::stoi(args[2]);
    auto caps = expand_cap(args[3]);
    edgecap::Capture cap;
    if (caps.empty() || !cap.load(caps[0])) return 2;
    edgecap::Machine m(cap);
    int k = 0;
    while (!m.done()) {
        if (m.c.pc == fn && loaded(m.c.ls) && k++ == which) {
            edgeunit::Snapshot ns;
            ns.ls.assign(m.ls.begin(), m.ls.end());
            ns.regs = m.c;
            ns.regs.ls = ns.ls.data();
            bool ok = natives().count(fn) && natives()[fn](ns.ls.data(), ns.regs);
            std::printf("native %s exit %05x sp %05x (entry sp %05x r0 %05x)\n", ok ? "ran" : "declined", ns.regs.gpr[0].u32[3], ns.regs.gpr[1].u32[3], m.c.gpr[1].u32[3], m.c.gpr[0].u32[3]);
            std::vector<uint32_t> last;
            for (int i = 0; i < 20000000 && !m.done(); i++) {
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

std::vector<PutRec> run_with_hooks(const std::string& path, const std::set<uint32_t>& hooks, uint64_t* hooked, uint64_t* skipped, uint64_t* declined, std::string* err, double* nat_sec, uint64_t* job_insns) {
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
        if (!hooks.empty() && hooks.count(pc) && loaded(m.c.ls)) {
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
                    if (job_insns) *job_insns += r.insns;
                    continue;
                }
                if (!r.ok && err && err->empty()) *err = "native ran where SPU is impure at hook " + std::to_string(pc);
                std::memcpy(m.ls.data(), orig_ls.data(), spu::LS_SIZE);
            } else if (declined)
                (*declined)++;
        }
        if (in_image(pc) && loaded(m.c.ls)) {
            if (g_hist) (*g_hist)[pc]++;
            if (job_insns) (*job_insns)++;
        }
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
    if (const char* skip = std::getenv("CS_SKIP"))
        for (uint32_t h : parse_list(skip)) hooks.erase(h);
    for (uint32_t h : hooks)
        if (!natives().count(h)) {
            std::printf("no native for %05x\n", h);
            return 2;
        }
    int bad = 0;
    std::map<uint32_t, uint64_t> hist;
    uint64_t t_job = 0, t_skip = 0;
    for (const std::string& path : cap_list(args, 2)) {
        g_hist = nullptr;
        std::vector<PutRec> ref = run_with_hooks(path, {}, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        uint64_t hooked = 0, skipped = 0, declined = 0, job_insns = 0;
        std::string err;
        double nat_sec = 0;
        if (std::getenv("CS_HIST")) g_hist = &hist;
        std::vector<PutRec> got = run_with_hooks(path, hooks, &hooked, &skipped, &declined, &err, &nat_sec, &job_insns);
        size_t mism = 0, job_puts = 0;
        std::map<uint32_t, size_t> mism_pc;
        for (size_t i = 0; i < std::min(ref.size(), got.size()); i++) {
            const PutRec& a = ref[i];
            const PutRec& b = got[i];
            if (in_image(a.pc)) job_puts++;
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
        std::printf("%-22s puts ref %zu got %zu (in-image pc %zu) mismatching %zu hooked %llu declined %llu replaced %llu of %llu job insns (%.1f%%) native %.1f ms %s\n", short_name(path).c_str(), ref.size(), got.size(), job_puts, mism, (unsigned long long)hooked, (unsigned long long)declined,
                    (unsigned long long)skipped, (unsigned long long)job_insns, 100.0 * skipped / std::max<uint64_t>(1, job_insns), nat_sec * 1e3, err.c_str());
        for (auto& [pc, n] : mism_pc) std::printf("    mismatches at pc %05x: %zu\n", pc, n);
        if (mism || ref.size() != got.size() || !err.empty()) bad++;
        t_job += job_insns;
        t_skip += skipped;
    }
    std::printf("total: replaced %llu of %llu job insns (%.2f%%), %d bad captures\n", (unsigned long long)t_skip, (unsigned long long)t_job, 100.0 * t_skip / std::max<uint64_t>(1, t_job), bad);
    if (const char* hf = std::getenv("CS_HIST"))
        if (std::FILE* f = std::fopen(hf, "w")) {
            for (auto& [pc, n] : hist) std::fprintf(f, "%05x %llu\n", pc, (unsigned long long)n);
            std::fclose(f);
        }
    return bad ? 1 : 0;
}

int cmd_args(const std::vector<std::string>& args) {
    if (args.size() < 2) return 2;
    uint32_t fn = std::stoul(args[1], nullptr, 16);
    int maxhits = std::getenv("CS_MAX") ? std::atoi(std::getenv("CS_MAX")) : 1000000;
    int n = 0;
    for_each_call(cap_list(args, 2), fn, [&](edgeunit::Hit& h) {
        std::printf("%s job %d insns %llu pure %d", short_name(h.cap).c_str(), h.job, (unsigned long long)h.fn_insns, h.pure);
        for (int r = 3; r <= 10; r++) std::printf(" %05x", h.before.regs.gpr[r].u32[3]);
        std::printf(" sp %05x r3out %08x\n", h.sp, h.after.regs.gpr[3].u32[3]);
        return ++n < maxhits;
    });
    return 0;
}

int cmd_dump(const std::vector<std::string>& args) {
    if (args.size() < 5) return 2;
    uint32_t fn = std::stoul(args[1], nullptr, 16);
    int which = std::stoi(args[2]);
    std::string out = args[3];
    int n = 0;
    for_each_call(cap_list(args, 4), fn, [&](edgeunit::Hit& h) {
        if (n++ < which) return true;
        if (std::FILE* f = std::fopen((out + ".before").c_str(), "wb")) {
            std::fwrite(h.before.regs.gpr, 1, 2048, f);
            std::fwrite(h.before.ls.data(), 1, spu::LS_SIZE, f);
            std::fclose(f);
        }
        if (std::FILE* f = std::fopen((out + ".after").c_str(), "wb")) {
            std::fwrite(h.after.regs.gpr, 1, 2048, f);
            std::fwrite(h.after.ls.data(), 1, spu::LS_SIZE, f);
            std::fclose(f);
        }
        std::printf("dumped %s job %d insns %llu\n", short_name(h.cap).c_str(), h.job, (unsigned long long)h.fn_insns);
        return false;
    });
    return 0;
}

std::vector<uint32_t> read_quads(edgeunit::Snapshot s, uint32_t ret, uint32_t sp, uint64_t max_insns) {
    class NullHostT : public spu::Host {
    public:
        bool failed = false;
        uint32_t read_channel(uint32_t) override {
            failed = true;
            return 0;
        }
        void write_channel(uint32_t, uint32_t) override { failed = true; }
        uint32_t channel_count(uint32_t) override {
            failed = true;
            return 0;
        }
        bool stop_and_signal(uint32_t) override {
            failed = true;
            return false;
        }
        void halt(uint32_t, uint32_t) override { failed = true; }
    };
    std::set<uint32_t> q;
    s.ls.resize(spu::LS_SIZE + 16);
    s.regs.ls = s.ls.data();
    s.regs.line_epoch = nullptr;
    NullHostT host;
    for (uint64_t k = 0; k < max_insns && !host.failed; k++) {
        uint32_t pc = s.regs.pc;
        uint32_t w = be32(s.ls.data() + pc);
        spudec::Insn in = spudec::decode(w);
        if (in.load) {
            uint32_t addr;
            if (in.pc_rel) addr = pc + in.imm * 4;
            else if (in.abs_addr) addr = in.imm * 4;
            else addr = s.regs.gpr[in.addr_a].u32[3] + (in.addr_b >= 0 ? s.regs.gpr[in.addr_b].u32[3] : uint32_t(in.imm));
            addr &= 0x3fff0;
            if (!in_image(addr)) q.insert(addr);
        }
        spu::step(s.regs, host);
        if (s.regs.pc == ret && s.regs.gpr[1].u32[3] == sp) break;
    }
    return std::vector<uint32_t>(q.begin(), q.end());
}

uint32_t mutate_word(uint32_t v, std::mt19937& rng) {
    static const uint32_t special[] = {0, 0x80000000u, 0x00000001u, 0x007fffffu, 0x00800000u, 0x7f800000u, 0xff800000u, 0x7f7fffffu, 0xff7fffffu, 0x3f800000u, 0xbf800000u, 0x7fffffffu, 0xffffffffu, 0x7fc00000u, 0x0000ffffu, 0x00010000u};
    switch (rng() % 8) {
    case 0: return special[rng() % (sizeof(special) / 4)];
    case 1: return v ^ (1u << (rng() % 32));
    case 2: return v + (rng() % 7) - 3;
    case 3: return v ^ 0x80000000u;
    case 4: return (v & 0x807fffffu) | ((rng() % 256) << 23);
    case 5: return rng();
    case 6: return v & ~(0xffu << (8 * (rng() % 4)));
    default: return (v & 0x807fffffu) | ((((v >> 23) & 0xff) + (rng() % 9) - 4) & 0xff) << 23;
    }
}

int cmd_mfuzz(const std::vector<std::string>& args) {
    if (args.size() < 2) return 2;
    std::vector<uint32_t> fns;
    if (args[1] == "all")
        for (auto& [k, v] : natives()) fns.push_back(k);
    else
        fns = parse_list(args[1]);
    int maxhits = args.size() > 2 ? std::stoi(args[2]) : 20;
    int trials = args.size() > 3 ? std::stoi(args[3]) : 20;
    uint32_t seed = args.size() > 4 ? std::stoul(args[4]) : 1;
    int bad_total = 0;
    for (uint32_t fn : fns) {
        NativeFn nat = natives()[fn];
        if (!nat) continue;
        std::mt19937 rng(seed ^ fn);
        int hits = 0, ran = 0, declined = 0, bad = 0, isofail = 0, smc = 0, longloop = 0;
        for (const std::string& path : cap_list(args, 5)) {
            edgecap::Capture cap;
            if (!cap.load(path)) continue;
            edgecap::Machine m(cap);
            while (!m.done() && hits < maxhits) {
                if (m.c.pc == fn && loaded(m.c.ls)) {
                    hits++;
                    edgeunit::Snapshot base;
                    base.ls.assign(m.ls.begin(), m.ls.begin() + spu::LS_SIZE + 16);
                    base.regs = m.c;
                    edgeunit::Snapshot probe = base;
                    probe.ls.resize(spu::LS_SIZE + 16);
                    probe.regs.ls = probe.ls.data();
                    bool ok0 = nat(probe.ls.data(), probe.regs);
                    uint32_t exit0 = probe.regs.gpr[0].u32[3] & spu::PC_MASK, sp0 = probe.regs.gpr[1].u32[3];
                    std::vector<uint32_t> quads = ok0 ? read_quads(base, exit0, sp0, 50000000) : std::vector<uint32_t>{};
                    for (int t = 0; t < trials && !quads.empty(); t++) {
                        edgeunit::Snapshot s = base;
                        int nm = 1 + int(rng() % 8);
                        for (int k = 0; k < nm; k++) {
                            uint32_t a = quads[rng() % quads.size()] + 4 * (rng() % 4);
                            uint32_t v = be32(s.ls.data() + a);
                            v = __builtin_bswap32(mutate_word(v, rng));
                            std::memcpy(s.ls.data() + a, &v, 4);
                        }
                        edgeunit::Snapshot ns = s;
                        ns.regs.ls = ns.ls.data();
                        bool ok = nat(ns.ls.data(), ns.regs);
                        if (!ok) {
                            declined++;
                            if (std::memcmp(ns.ls.data(), s.ls.data(), spu::LS_SIZE) != 0 || regs_diff(ns.regs, s.regs, false) >= 0) {
                                bad++;
                                if (bad <= 5) std::printf("  %05x hit %d trial %d: decline left changes\n", fn, hits, t);
                            }
                            continue;
                        }
                        uint32_t exit_pc = ns.regs.gpr[0].u32[3] & spu::PC_MASK, exit_sp = ns.regs.gpr[1].u32[3];
                        bool region = exit_pc != (s.regs.gpr[0].u32[3] & spu::PC_MASK);
                        edgeunit::Snapshot iso = s;
                        edgeunit::IsoResult r = edgeunit::run_isolated(iso, exit_pc, exit_sp, 200000000);
                        if (std::memcmp(iso.ls.data() + kImageLs, g_image.data(), g_image.size()) != 0) {
                            smc++;
                            continue;
                        }
                        if (!r.ok) {
                            isofail++;
                            bad++;
                            if (bad <= 5) std::printf("  %05x hit %d trial %d: native exit %05x sp %05x not reached by SPU (%s)\n", fn, hits, t, exit_pc, exit_sp, r.error.c_str());
                            continue;
                        }
                        ran++;
                        edgeunit::LsDiff d = edgeunit::diff_ls(ns.ls.data(), iso.ls.data(), 0, 0);
                        int rd = regs_diff(ns.regs, iso.regs, region);
                        if ((d.bytes || rd >= 0) && region) {
                            edgeunit::Snapshot lp = s;
                            edgeunit::IsoResult lr = edgeunit::run_isolated(lp, s.regs.gpr[0].u32[3] & spu::PC_MASK, s.regs.gpr[1].u32[3], 150000000);
                            if (!lr.ok) {
                                ran--;
                                longloop++;
                                continue;
                            }
                        }
                        if (d.bytes || rd >= 0) {
                            bad++;
                            if (bad <= 5) std::printf("  %05x hit %d trial %d: ls diff %zu [%05x..%05x] reg r%d\n", fn, hits, t, d.bytes, d.first, d.last, rd);
                            if (const char* dp = std::getenv("CS_DUMP"); dp && bad == 1) {
                                if (std::FILE* f = std::fopen(dp, "wb")) {
                                    std::fwrite(s.regs.gpr, 1, 2048, f);
                                    std::fwrite(&s.regs.pc, 1, 4, f);
                                    std::fwrite(s.ls.data(), 1, spu::LS_SIZE, f);
                                    std::fclose(f);
                                }
                            }
                            if (std::getenv("CS_VERBOSE") && bad <= 2) {
                                int shown = 0;
                                for (uint32_t q = 0; q < spu::LS_SIZE && shown < 16; q += 16)
                                    if (std::memcmp(ns.ls.data() + q, iso.ls.data() + q, 16)) {
                                        std::printf("    %05x nat %08x %08x %08x %08x spu %08x %08x %08x %08x orig %08x %08x %08x %08x\n", q, be32(&ns.ls[q]), be32(&ns.ls[q + 4]), be32(&ns.ls[q + 8]), be32(&ns.ls[q + 12]), be32(&iso.ls[q]), be32(&iso.ls[q + 4]), be32(&iso.ls[q + 8]), be32(&iso.ls[q + 12]), be32(&s.ls[q]), be32(&s.ls[q + 4]), be32(&s.ls[q + 8]), be32(&s.ls[q + 12]));
                                        shown++;
                                    }
                                std::printf("    iso insns %llu\n", (unsigned long long)r.insns);
                            }
                        }
                    }
                }
                m.step();
            }
        }
        std::printf("mfuzz %05x: hits %d ran %d declined %d iso-fail %d self-modified %d long-loop %d bad %d\n", fn, hits, ran, declined, isofail, smc, longloop, bad);
        bad_total += bad;
    }
    return bad_total ? 1 : 0;
}

int cmd_dcheck(const std::vector<std::string>& args) {
    if (args.size() < 2) return 2;
    std::vector<uint8_t> raw = read_file(args[1]);
    if (raw.size() < 2048 + 4 + spu::LS_SIZE) return 2;
    edgeunit::Snapshot s;
    s.ls.assign(spu::LS_SIZE + 16, 0);
    std::memcpy(s.regs.gpr, raw.data(), 2048);
    std::memcpy(&s.regs.pc, raw.data() + 2048, 4);
    std::memcpy(s.ls.data(), raw.data() + 2052, spu::LS_SIZE);
    s.regs.ls = s.ls.data();
    class NullHostT : public spu::Host {
    public:
        uint32_t read_channel(uint32_t) override { return 0; }
        void write_channel(uint32_t, uint32_t) override {}
        uint32_t channel_count(uint32_t) override { return 0; }
        bool stop_and_signal(uint32_t) override { return false; }
        void halt(uint32_t, uint32_t) override {}
    } host;
    int reported = 0;
    uint64_t k = 0;
    uint32_t ret0 = s.regs.gpr[0].u32[3], sp0 = s.regs.gpr[1].u32[3];
    std::printf("dcheck pc %05x ret %05x sp %05x has %d\n", s.regs.pc, ret0, sp0, int(cs_has_gen(g_job->name, s.regs.pc)));
    {
        edgeunit::Snapshot ns = s;
        ns.regs.ls = ns.ls.data();
        NativeFn nat = natives().count(s.regs.pc) ? natives()[s.regs.pc] : nullptr;
        if (nat && nat(ns.ls.data(), ns.regs)) {
            edgeunit::Snapshot iso = s;
            edgeunit::IsoResult r = edgeunit::run_isolated(iso, ns.regs.gpr[0].u32[3] & spu::PC_MASK, ns.regs.gpr[1].u32[3], 100000000);
            edgeunit::LsDiff d = edgeunit::diff_ls(ns.ls.data(), iso.ls.data(), 0, 0);
            std::printf("adapter native: exit %05x sp %05x iso %d insns %llu ls diff %zu reg r%d\n", ns.regs.gpr[0].u32[3], ns.regs.gpr[1].u32[3], int(r.ok), (unsigned long long)r.insns, d.bytes, regs_diff(ns.regs, iso.regs, false));
        }
        edgeunit::Snapshot iso2 = s;
        edgeunit::IsoResult r2 = edgeunit::run_isolated(iso2, ret0, sp0, 300000000);
        std::printf("plain iso to ret: ok %d insns %llu (%s)\n", int(r2.ok), (unsigned long long)r2.insns, r2.error.c_str());
    }
    while (k < 100000000 && reported < 6) {
        uint32_t pc = s.regs.pc;
        if (cs_has_gen(g_job->name, pc)) {
            edgeunit::Snapshot ns = s;
            ns.regs.ls = ns.ls.data();
            bool ok = cs_gen_call(g_job->name, pc, ns.ls.data(), ns.regs);
            if (k < 3 || std::getenv("CS_VERBOSE")) std::printf("gen %05x at %llu ok %d\n", pc, (unsigned long long)k, int(ok));
            if (ok) {
                uint32_t ep = ns.regs.gpr[0].u32[3] & spu::PC_MASK, esp = ns.regs.gpr[1].u32[3];
                edgeunit::Snapshot iso = s;
                edgeunit::IsoResult r = edgeunit::run_isolated(iso, ep, esp, 100000000);
                edgeunit::LsDiff d = edgeunit::diff_ls(ns.ls.data(), iso.ls.data(), 0, 0);
                int rd = r.ok ? regs_diff(ns.regs, iso.regs, ep != (s.regs.gpr[0].u32[3] & spu::PC_MASK)) : -2;
                if (!r.ok || d.bytes || rd >= 0) {
                    std::printf("call %05x at insn %llu: iso %s insns %llu ls diff %zu [%05x..%05x] reg r%d\n", pc, (unsigned long long)k, r.ok ? "ok" : r.error.c_str(), (unsigned long long)r.insns, d.bytes, d.first, d.last, rd);
                    if (rd >= 0) std::printf("   r%d nat %08x %08x %08x %08x spu %08x %08x %08x %08x\n", rd, ns.regs.gpr[rd].u32[3], ns.regs.gpr[rd].u32[2], ns.regs.gpr[rd].u32[1], ns.regs.gpr[rd].u32[0], iso.regs.gpr[rd].u32[3], iso.regs.gpr[rd].u32[2], iso.regs.gpr[rd].u32[1], iso.regs.gpr[rd].u32[0]);
                    if (std::getenv("CS_DUMP2") && reported == 0)
                        if (std::FILE* f = std::fopen(std::getenv("CS_DUMP2"), "wb")) {
                            std::fwrite(s.regs.gpr, 1, 2048, f);
                            std::fwrite(&s.regs.pc, 1, 4, f);
                            std::fwrite(s.ls.data(), 1, spu::LS_SIZE, f);
                            std::fclose(f);
                        }
                    reported++;
                }
            }
        }
        spu::step(s.regs, host);
        if (k < 5 || (k % 10000000) == 0) std::printf("step %llu pc %05x\n", (unsigned long long)k, s.regs.pc);
        k++;
        if (s.regs.pc == ret0 && s.regs.gpr[1].u32[3] == sp0) break;
    }
    std::printf("dcheck done after %llu insns\n", (unsigned long long)k);
    return reported ? 1 : 0;
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
        edgeunit::Snapshot ns = h.before;
        ns.ls.resize(spu::LS_SIZE + 16);
        for (int k = 0; k < 20; k++) {
            std::memcpy(ns.ls.data(), h.before.ls.data(), spu::LS_SIZE);
            ns.regs = h.before.regs;
            ns.regs.ls = ns.ls.data();
            auto t0 = std::chrono::steady_clock::now();
            nat(ns.ls.data(), ns.regs);
            best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        }
        best_total += best;
        insns += h.fn_insns;
        return ++n < maxhits;
    });
    std::printf("fn %05x: %d hits, %.3f us/call best, %.3f ns per SPU insn\n", fn, n, n ? best_total * 1e6 / n : 0.0, insns ? best_total * 1e9 / insns : 0.0);
    if (std::getenv("CS_PROF_DUMP")) csprof::dump();
    return 0;
}

int cmd_rbench(const std::vector<std::string>& args) {
    if (args.size() < 3) return 2;
    uint32_t fn = std::stoul(args[1], nullptr, 16);
    int maxhits = std::stoi(args[2]);
    NativeFn nat = natives()[fn];
    if (!nat) return 2;
    double total = 0;
    uint64_t insns = 0;
    int n = 0;
    for (const std::string& path : cap_list(args, 3)) {
        edgecap::Capture cap;
        if (!cap.load(path)) continue;
        edgecap::Machine m(cap);
        while (!m.done() && n < maxhits) {
            if (m.c.pc == fn && loaded(m.c.ls)) {
                std::vector<uint8_t> ls(m.ls.begin(), m.ls.end());
                double best = 1e9;
                spu::Context c;
                for (int k = 0; k < 20; k++) {
                    std::memcpy(ls.data(), m.ls.data(), spu::LS_SIZE);
                    c = m.c;
                    c.ls = ls.data();
                    auto t0 = std::chrono::steady_clock::now();
                    nat(ls.data(), c);
                    best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
                }
                edgeunit::Snapshot iso;
                iso.ls.assign(m.ls.begin(), m.ls.begin() + spu::LS_SIZE);
                iso.regs = m.c;
                edgeunit::IsoResult r = edgeunit::run_isolated(iso, c.gpr[0].u32[3] & spu::PC_MASK, c.gpr[1].u32[3]);
                if (r.ok) insns += r.insns;
                total += best;
                n++;
            }
            m.step();
        }
    }
    std::printf("fn %05x: %d hits, %.3f us/hit best (hot), %.3f ns per SPU insn\n", fn, n, n ? total * 1e6 / n : 0.0, insns ? total * 1e9 / insns : 0.0);
    return 0;
}

}

int main(int argc, char** argv) {
    _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x6000u | 0x8040u);
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        std::fprintf(stderr, "usage: cs_harness cloth|shadows <cmd> ...\n");
        return 2;
    }
    for (const JobDesc& j : kJobs)
        if (args[0] == j.name) g_job = &j;
    if (!g_job) {
        std::fprintf(stderr, "unknown job %s\n", args[0].c_str());
        return 2;
    }
    args.erase(args.begin());
    std::string root = GOWA_ROOT;
    g_elf = read_file(root + "/bin/01.12/GOWA.patched.elf");
    if (g_elf.size() < g_job->ea - 0x10000 + g_job->size) {
        std::fprintf(stderr, "cannot read elf\n");
        return 2;
    }
    g_image.assign(g_elf.begin() + (g_job->ea - 0x10000), g_elf.begin() + (g_job->ea - 0x10000) + g_job->size);
    g_code_end = kImageLs + g_job->size;
    std::string cmd = args.empty() ? "replay" : args[0];
    if (cmd == "replay") return cmd_replay(args);
    if (cmd == "calls") return cmd_calls(args);
    if (cmd == "hist") return cmd_hist(args);
    if (cmd == "trace") return cmd_trace(args);
    if (cmd == "unit") return cmd_unit(args);
    if (cmd == "rtrace") return cmd_rtrace(args);
    if (cmd == "itrace") return cmd_itrace(args);
    if (cmd == "e2e") return cmd_e2e(args);
    if (cmd == "args") return cmd_args(args);
    if (cmd == "dump") return cmd_dump(args);
    if (cmd == "bench") return cmd_bench(args);
    if (cmd == "mfuzz") return cmd_mfuzz(args);
    if (cmd == "dcheck") return cmd_dcheck(args);
    if (cmd == "rbench") return cmd_rbench(args);
    if (cmd == "fuzz") return cs_fuzz(g_job->name, g_image, args);
    if (cmd == "opfuzz") return cs_opfuzz(g_job->name, g_image, args);
    if (cmd == "convfuzz") return cs_convfuzz(args);
    std::fprintf(stderr, "unknown command\n");
    return 2;
}
