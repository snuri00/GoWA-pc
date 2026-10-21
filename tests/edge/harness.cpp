#include "capture.h"
#include "edge_native.h"
#include "spu_decode.h"
#include "unit.h"
#include "natreg.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <xmmintrin.h>

namespace {

constexpr uint32_t kImageFileOffset = 0x727580;
constexpr uint32_t kImageSize = 0xc430;
constexpr uint32_t kImageLs = 0x4000;
constexpr uint32_t kEntry = 0xad98;

std::vector<uint8_t> g_elf;
std::vector<uint8_t> g_image;

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

bool edge_loaded(const uint8_t* ls) { return std::memcmp(ls + kImageLs, g_image.data(), 0x40) == 0; }

std::string cap_path(const std::string& s) {
    if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos) return s;
    return std::string(GOWA_ROOT) + "/out/spucap/" + s + (s.find(".bin") == std::string::npos ? ".bin" : "");
}

int cmd_replay(const std::vector<std::string>& args) {
    int bad = 0;
    for (size_t i = 1; i < args.size(); i++) {
        edgecap::Capture cap;
        if (!cap.load(cap_path(args[i]))) {
            std::printf("%s: bad capture\n", args[i].c_str());
            bad++;
            continue;
        }
        edgecap::Machine m(cap);
        uint64_t edge_insns = 0, entries = 0;
        while (!m.done()) {
            if (m.c.pc == kEntry && edge_loaded(m.c.ls)) entries++;
            if (m.c.pc >= 0x4000 && m.c.pc < 0x29000 && edge_loaded(m.c.ls)) edge_insns++;
            m.step();
        }
        std::string diff;
        bool ok = m.verify_final(diff);
        std::printf("%s: insns %llu edge-entries %llu edge-insns %llu %s%s\n", args[i].c_str(), (unsigned long long)m.n, (unsigned long long)entries, (unsigned long long)edge_insns, ok ? "exact" : "DIFF", diff.c_str());
        bad += !ok;
    }
    return bad ? 1 : 0;
}

struct Callback {
    std::string name;
    uint32_t ea = 0;
    uint32_t size = 0;
};

std::vector<Callback> g_callbacks;

uint32_t fnv1(const std::string& s) {
    uint32_t h = 0x811c9dc5u;
    for (unsigned char ch : s) h = (h * 0x01000193u) ^ ch;
    return h;
}

void load_callbacks() {
    std::map<uint32_t, std::string> names;
    const char* pre[] = {"smDecompressVertices_", "smCompressVertices_", "smSetVertexDataArrays_"};
    for (auto p : pre)
        for (int i = 0; i < 32; i++) names[fnv1(std::string(p) + std::to_string(i))] = std::string(p) + std::to_string(i);
    uint32_t off = 0x754e00 - 0x10000;
    uint32_t cnt = (g_elf[off] << 8) | g_elf[off + 1];
    uint32_t doff = (g_elf[off + 2] << 8) | g_elf[off + 3];
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t* e = g_elf.data() + off + 4 + 8 * i;
        uint32_t h = be32(e);
        Callback cb;
        char b[32];
        std::snprintf(b, sizeof b, "hash_%08x", h);
        cb.name = names.count(h) ? names[h] : b;
        cb.ea = 0x754e00 + doff + 16 * ((e[4] << 8) | e[5]);
        cb.size = (e[6] << 8) | e[7];
        g_callbacks.push_back(cb);
    }
}

struct Placement {
    uint32_t lsa;
    int cb;
};

std::vector<Placement> find_overlays(const uint8_t* ls) {
    std::vector<Placement> out;
    for (uint32_t a = 0x11000; a < 0x29000; a += 16) {
        for (size_t k = 0; k < g_callbacks.size(); k++) {
            const Callback& cb = g_callbacks[k];
            const uint8_t* img = g_elf.data() + cb.ea - 0x10000;
            if (a + cb.size > 0x29000) continue;
            if (std::memcmp(ls + a, img, 16) != 0) continue;
            if (std::memcmp(ls + a, img, cb.size) != 0) continue;
            out.push_back({a, int(k)});
        }
    }
    return out;
}

int overlay_at(const std::vector<Placement>& pl, uint32_t pc, uint32_t* off = nullptr) {
    for (const Placement& p : pl)
        if (pc >= p.lsa && pc < p.lsa + g_callbacks[p.cb].size) {
            if (off) *off = pc - p.lsa;
            return p.cb;
        }
    return -1;
}

struct FnStat {
    uint64_t calls = 0;
    uint64_t incl = 0;
    uint64_t excl = 0;
    uint64_t impure = 0;
    std::set<uint32_t> callers;
};

std::string fn_name(uint32_t fn, const std::vector<Placement>& pl) {
    char b[96];
    uint32_t off = 0;
    int k = fn >= 0x11000 && fn < 0x29000 ? overlay_at(pl, fn, &off) : -1;
    if (k >= 0) std::snprintf(b, sizeof b, "%s+%x", g_callbacks[k].name.c_str(), off);
    else std::snprintf(b, sizeof b, "%05x", fn);
    return b;
}

int cmd_calls(const std::vector<std::string>& args) {
    std::map<std::string, FnStat> stats;
    std::map<std::string, uint64_t> overlay_insns, overlay_jobs;
    uint64_t total_edge = 0, jobs = 0;
    std::map<std::string, uint64_t> dmas;
    for (size_t i = 1; i < args.size(); i++) {
        edgecap::Capture cap;
        if (!cap.load(cap_path(args[i]))) continue;
        edgecap::Machine m(cap);
        struct Frame {
            std::string fn;
            uint32_t ret;
            uint64_t start, child;
            bool impure;
        };
        std::vector<Frame> st;
        bool in_job = false;
        std::vector<Placement> pl;
        bool pl_valid = false;
        std::set<int> used;
        auto mark = [&]() {
            for (Frame& f : st) f.impure = true;
        };
        m.on_dma = [&](const edgecap::Dma& d, bool after) {
            if (!in_job || after) return;
            char b[128];
            std::snprintf(b, sizeof b, "cmd %02x pc %05x", d.cmd, d.pc);
            dmas[b]++;
        };
        size_t last_pos = 0;
        while (!m.done()) {
            uint32_t pc = m.c.pc;
            if (!in_job && pc == kEntry && edge_loaded(m.c.ls)) {
                in_job = true;
                jobs++;
                st.clear();
                st.push_back({"0ad98", m.c.gpr[0].u32[3], m.n, 0, false});
                pl_valid = false;
                used.clear();
            }
            if (!in_job) {
                m.step();
                continue;
            }
            total_edge++;
            if (pc >= 0x11000 && pc < 0x29000) {
                if (!pl_valid) {
                    pl = find_overlays(m.c.ls);
                    pl_valid = true;
                }
                int k = overlay_at(pl, pc);
                std::string nm = k >= 0 ? g_callbacks[k].name : "unknown-buffer-code";
                overlay_insns[nm]++;
                if (k >= 0 && used.insert(k).second) overlay_jobs[nm]++;
            }
            uint32_t w = be32(m.c.ls + pc);
            spudec::Insn in = spudec::decode(w);
            last_pos = m.pos;
            m.step();
            if (m.pos != last_pos) {
                const char* nm = in.name;
                if (spudec::any_of(nm, {"rdch", "wrch", "rchcnt", "stop", "stopd"})) mark();
            }
            uint32_t npc = m.c.pc;
            bool call = (spudec::is(in.name, "brsl") || spudec::is(in.name, "brasl") || spudec::is(in.name, "bisl")) && npc != pc + 4 && in.dst >= 0 && m.c.gpr[in.dst].u32[3] == pc + 4;
            if (call) {
                if (npc >= 0x11000 && npc < 0x29000 && !pl_valid) {
                    pl = find_overlays(m.c.ls);
                    pl_valid = true;
                }
                std::string nm = fn_name(npc, pl);
                st.push_back({nm, pc + 4, m.n, 0, false});
                stats[nm].callers.insert(pc);
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
    std::vector<std::pair<std::string, FnStat>> v(stats.begin(), stats.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.incl > b.second.incl; });
    std::printf("jobs %llu edge insns %llu (%.0f per job)\n", (unsigned long long)jobs, (unsigned long long)total_edge, jobs ? double(total_edge) / jobs : 0.0);
    for (auto& [fn, s] : v) {
        std::printf("fn %-28s calls %7llu incl %10llu (%5.1f%%) excl %10llu (%5.1f%%) per-call %8.1f impure %6llu callers", fn.c_str(), (unsigned long long)s.calls, (unsigned long long)s.incl, 100.0 * s.incl / total_edge, (unsigned long long)s.excl,
                    100.0 * s.excl / total_edge, double(s.incl) / s.calls, (unsigned long long)s.impure);
        int k = 0;
        for (uint32_t c : s.callers) {
            if (k++ > 5) break;
            std::printf(" %05x", c);
        }
        std::printf("\n");
    }
    for (auto& [k, n] : overlay_insns) std::printf("overlay %-28s insns %10llu (%5.1f%%) jobs %llu\n", k.c_str(), (unsigned long long)n, 100.0 * n / total_edge, (unsigned long long)overlay_jobs[k]);
    for (auto& [k, n] : dmas) std::printf("dma %s x%llu\n", k.c_str(), (unsigned long long)n);
    return 0;
}

class NullHostT : public spu::Host {
public:
    uint32_t read_channel(uint32_t) override { return 0; }
    void write_channel(uint32_t, uint32_t) override {}
    uint32_t channel_count(uint32_t) override { return 0; }
    bool stop_and_signal(uint32_t) override { return false; }
    void halt(uint32_t, uint32_t) override {}
};

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
    while (!m.done()) {
        uint32_t pc = m.c.pc;
        if (!in_job && pc == kEntry && edge_loaded(m.c.ls)) {
            in_job = true;
            job++;
            st.clear();
            st.push_back({kEntry, m.c.gpr[0].u32[3]});
            if (job == want) {
                std::printf("job %d at insn %llu\n", job, (unsigned long long)m.n);
                print_regs(m.c, 1, 1 + regs, "  ");
            }
        }
        if (!in_job) {
            m.step();
            continue;
        }
        uint32_t w = be32(m.c.ls + pc);
        spudec::Insn in = spudec::decode(w);
        uint64_t n0 = m.n;
        m.step();
        uint32_t npc = m.c.pc;
        bool call = (spudec::is(in.name, "brsl") || spudec::is(in.name, "brasl") || spudec::is(in.name, "bisl")) && npc != pc + 4 && in.dst >= 0 && m.c.gpr[in.dst].u32[3] == pc + 4;
        if (call) {
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

std::map<uint32_t, NativeFn>& natives_store() {
    static std::map<uint32_t, NativeFn> m = [] {
        std::map<uint32_t, NativeFn> t;
        size_t n = 0;
        const edge::Adapter* a = edge::adapters(n);
        for (size_t i = 0; i < n; i++) t[a[i].lsa] = a[i].handler;
        return t;
    }();
    return m;
}

std::vector<std::string> cap_list(const std::vector<std::string>& args, size_t from) {
    std::vector<std::string> v;
    for (size_t i = from; i < args.size(); i++) {
        if (args[i] == "all" || args[i] == "rj" || args[i] == "rj2" || args[i] == "base") {
            std::vector<std::string> dirs;
            if (args[i] == "all" || args[i] == "base") dirs.push_back("spucap");
            if (args[i] == "all" || args[i] == "rj") dirs.push_back("spucap_rj");
            if (args[i] == "all" || args[i] == "rj2") dirs.push_back("spucap_rj2");
            for (auto& d : dirs)
                for (int k = 0; k < 4; k++) v.push_back(std::string(GOWA_ROOT) + "/out/" + d + "/cap_" + std::to_string(k) + ".bin");
        } else
            v.push_back(cap_path(args[i]));
    }
    if (v.empty())
        for (int i = 0; i < 4; i++) v.push_back(cap_path("cap_" + std::to_string(i)));
    return v;
}




void write_snapshot(std::FILE* f, const edgeunit::Snapshot& s) {
    std::fwrite(s.regs.gpr, 16, 128, f);
    std::fwrite(s.ls.data(), 1, spu::LS_SIZE, f);
}

int cmd_dump(const std::vector<std::string>& args) {
    if (args.size() < 4) return 2;
    uint32_t fn = std::stoul(args[1], nullptr, 16);
    int maxhits = std::stoi(args[2]);
    std::string out = args[3];
    std::FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) return 2;
    int n = 0;
    edgeunit::for_each_call(cap_list(args, 4), fn, [&](edgeunit::Hit& h) {
        uint32_t hdr[8] = {0x45444745u, h.fn, h.ret, h.sp, uint32_t(h.job), uint32_t(h.fn_insns), h.pure ? 1u : 0u, 0};
        std::fwrite(hdr, 4, 8, f);
        write_snapshot(f, h.before);
        write_snapshot(f, h.after);
        return ++n < maxhits;
    }, edge_loaded);
    std::fclose(f);
    std::printf("dumped %d hits\n", n);
    return 0;
}

int cmd_unit(const std::vector<std::string>& args) {
    if (args.size() < 2) return 2;
    uint32_t fn = std::stoul(args[1], nullptr, 16);
    int maxhits = args.size() > 2 ? std::stoi(args[2]) : 1000000;
    bool verbose = std::getenv("EDGE_VERBOSE") != nullptr;
    NativeFn nat = natives().count(fn) ? natives()[fn] : nullptr;
    int hits = 0, iso_bad = 0, nat_bad = 0, impure = 0, scratch_hits = 0, stack_hits = 0, declined = 0;
    uint64_t scratch_bytes = 0;
    uint64_t spu_insns = 0;
    double nat_sec = 0;
    edgeunit::for_each_call(cap_list(args, 3), fn, [&](edgeunit::Hit& h) {
        hits++;
        spu_insns += h.fn_insns;
        if (!h.pure) impure++;
        edgeunit::Snapshot iso = h.before;
        edgeunit::IsoResult r = edgeunit::run_isolated(iso, h.ret, h.sp);
        edgeunit::LsDiff d = edgeunit::diff_ls(iso.ls.data(), h.after.ls.data(), h.sp);
        bool r3ok = std::memcmp(&iso.regs.gpr[3], &h.after.regs.gpr[3], 16) == 0;
        if (!r.ok || d.bytes || !r3ok) {
            iso_bad++;
            if (iso_bad <= 5) std::printf("  iso mismatch job %d insn %llu: %s ls_diff %zu [%05x..%05x] r3 %s\n", h.job, (unsigned long long)h.insn, r.ok ? "ok" : r.error.c_str(), d.bytes, d.first, d.last, r3ok ? "ok" : "DIFF");
        }
        if (nat) {
            edgeunit::Snapshot ns = h.before;
            ns.ls.resize(spu::LS_SIZE + 16);
            ns.regs.ls = ns.ls.data();
            auto t0 = std::chrono::steady_clock::now();
            edge::Outputs outs;
            bool ok = nat(ns.ls.data(), ns.regs, outs);
            nat_sec += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (!ok) {
                declined++;
                return hits < maxhits;
            }
            edgeunit::LsDiff dfull = edgeunit::diff_ls(ns.ls.data(), h.after.ls.data(), h.sp);
            scratch_bytes += dfull.bytes;
            if (edgeunit::diff_ls(ns.ls.data(), h.after.ls.data(), 0, 0).bytes) stack_hits++;
            if (dfull.bytes) scratch_hits++;
            edgeunit::LsDiff dn;
            for (int k = 0; k < outs.n; k++)
                for (uint32_t a = outs.r[k].lo; a < outs.r[k].hi; a++)
                    if (ns.ls[a & 0x3ffff] != h.after.ls[a & 0x3ffff]) {
                        if (!dn.bytes) dn.first = a;
                        dn.last = a;
                        dn.bytes++;
                    }
            bool n3 = std::memcmp(&ns.regs.gpr[3], &h.after.regs.gpr[3], 16) == 0;
            if (verbose && dfull.bytes) {
                std::printf("  scratch diff job %d:", h.job);
                uint32_t lo = h.sp > 0x10000 ? h.sp - 0x10000 : 0;
                for (uint32_t a = 0; a < spu::LS_SIZE;) {
                    if ((a >= lo && a < h.sp) || ns.ls[a] == h.after.ls[a]) {
                        a++;
                        continue;
                    }
                    uint32_t b = a;
                    while (b < spu::LS_SIZE && (ns.ls[b] != h.after.ls[b] || (b + 1 < spu::LS_SIZE && ns.ls[b + 1] != h.after.ls[b + 1]) || (b + 2 < spu::LS_SIZE && ns.ls[b + 2] != h.after.ls[b + 2]))) b++;
                    std::printf(" [%05x,%05x)", a, b);
                    a = b;
                }
                std::printf("\n");
            }
            if (!ok || dn.bytes || !n3) {
                nat_bad++;
                if (nat_bad <= 8 || verbose) {
                    std::printf("  native mismatch %s job %d insn %llu: %s ls_diff %zu [%05x..%05x] r3 %s", h.cap.c_str(), h.job, (unsigned long long)h.insn, ok ? "ran" : "declined", dn.bytes, dn.first, dn.last, n3 ? "ok" : "DIFF");
                    if (!n3) std::printf(" (%08x vs %08x)", ns.regs.gpr[3].u32[3], h.after.regs.gpr[3].u32[3]);
                    std::printf("\n");
                }
            }
        }
        return hits < maxhits;
    }, edge_loaded);
    std::printf("fn %05x: hits %d impure %d spu-insns %llu (%.0f/call) iso-mismatch %d", fn, hits, impure, (unsigned long long)spu_insns, hits ? double(spu_insns) / hits : 0.0, iso_bad);
    if (nat) std::printf(" native-mismatch %d native-time %.3f ms (%.1f us/call) full-ls-diff hits %d bytes %llu incl-stack-diff hits %d declined %d", nat_bad, nat_sec * 1e3, hits ? nat_sec * 1e6 / hits : 0.0, scratch_hits, (unsigned long long)scratch_bytes, stack_hits, declined);
    std::printf("\n");
    return (iso_bad || nat_bad) ? 1 : 0;
}

int cmd_itrace(const std::vector<std::string>& args) {
    if (args.size() < 4) return 2;
    uint32_t fn = std::stoul(args[1], nullptr, 16);
    int which = std::stoi(args[2]);
    std::string mode = args[3];
    int n = 0;
    edgeunit::for_each_call(cap_list(args, 4), fn, [&](edgeunit::Hit& h) {
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
    }, edge_loaded);
    return 0;
}

struct PutRec {
    uint32_t cmd = 0, pc = 0, lsa = 0, ea = 0, size = 0;
    std::vector<uint8_t> data;
};

std::vector<PutRec> run_with_hooks(const std::string& path, const std::set<uint32_t>& hooks, bool strict, uint64_t* hooked, uint64_t* skipped, std::string* err) {
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
            uint32_t lsa = d.lsa;
            for (uint32_t k = 0; k < d.size; k += 8) {
                uint32_t sz = be32(m.ls.data() + list + k) & 0x7fff;
                for (uint32_t i = 0; i < sz; i++) p.data.push_back(m.ls[(lsa + i) & 0x3ffff]);
                lsa += sz;
            }
        } else if (d.size <= 0x4000)
            for (uint32_t i = 0; i < d.size; i++) p.data.push_back(m.ls[(d.lsa + i) & 0x3ffff]);
        puts.push_back(std::move(p));
    };
    while (!m.done()) {
        uint32_t pc = m.c.pc;
        if (!hooks.empty() && hooks.count(pc) && edge_loaded(m.c.ls)) {
            NativeFn nat = natives()[pc];
            edgeunit::Snapshot iso;
            iso.ls.assign(m.ls.begin(), m.ls.begin() + spu::LS_SIZE);
            iso.regs = m.c;
            uint32_t ret = m.c.gpr[0].u32[3], sp = m.c.gpr[1].u32[3];
            edgeunit::IsoResult r = edgeunit::run_isolated(iso, ret, sp);
            if (r.ok && m.n + r.insns < cap.insns) {
                spu::Context nc = m.c;
                edge::Outputs outs;
                if (nat(m.ls.data(), nc, outs)) {
                    spu::v128 r3 = nc.gpr[3];
                    if (strict) {
                        m.c.gpr[3] = r3;
                        if (const char* k = std::getenv("EDGE_STRICT_ISO")) {
                            int lo = 0, hi = -1;
                            std::sscanf(k, "%d-%d", &lo, &hi);
                            for (int i = lo; i <= hi && i < 128; i++) m.c.gpr[i] = iso.regs.gpr[i];
                        }
                    }
                    else {
                        uint8_t* keep = m.c.ls;
                        m.c = iso.regs;
                        m.c.ls = keep;
                        if (std::memcmp(&r3, &iso.regs.gpr[3], 16) != 0 && err && err->empty()) *err = "r3 differs at hook " + std::to_string(pc);
                    }
                    m.c.pc = ret;
                    m.skip(r.insns);
                    if (hooked) (*hooked)++;
                    if (skipped) *skipped += r.insns;
                    continue;
                }
            }
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
    std::string list = args[1];
    bool strict = std::getenv("EDGE_STRICT") != nullptr;
    if (list == "all") {
        for (auto& [k, v] : natives()) hooks.insert(k);
    } else {
        size_t p = 0;
        while (p < list.size()) {
            size_t e = list.find(',', p);
            if (e == std::string::npos) e = list.size();
            hooks.insert(std::stoul(list.substr(p, e - p), nullptr, 16));
            p = e + 1;
        }
    }
    for (uint32_t h : hooks)
        if (!natives().count(h)) {
            std::printf("no native for %05x\n", h);
            return 2;
        }
    int bad = 0;
    for (const std::string& path : cap_list(args, 2)) {
        std::vector<PutRec> ref = run_with_hooks(path, {}, false, nullptr, nullptr, nullptr);
        uint64_t hooked = 0, skipped = 0;
        std::string err;
        std::vector<PutRec> got = run_with_hooks(path, hooks, strict, &hooked, &skipped, &err);
        size_t mism = 0, edge_puts = 0;
        std::map<uint32_t, size_t> mism_pc;
        for (size_t i = 0; i < std::min(ref.size(), got.size()); i++) {
            const PutRec& a = ref[i];
            const PutRec& b = got[i];
            if (a.pc == 0xafc8 || a.pc == 0xd0fc || a.pc == 0xc95c) edge_puts++;
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
        std::printf("%s: puts ref %zu got %zu (edge %zu) mismatching %zu hooked %llu skipped-insns %llu %s\n", path.c_str(), ref.size(), got.size(), edge_puts, mism, (unsigned long long)hooked, (unsigned long long)skipped, err.c_str());
        for (auto& [pc, n] : mism_pc) std::printf("    mismatches at pc %05x: %zu\n", pc, n);
        if (mism || ref.size() != got.size() || !err.empty()) bad++;
    }
    return bad ? 1 : 0;
}

int cmd_args(const std::vector<std::string>& args) {
    if (args.size() < 2) return 2;
    uint32_t fn = std::stoul(args[1], nullptr, 16);
    uint32_t ctxreg = std::getenv("EDGE_CTXREG") ? std::stoul(std::getenv("EDGE_CTXREG")) : 3;
    edgeunit::for_each_call(cap_list(args, 2), fn, [&](edgeunit::Hit& h) {
        std::printf("%s job %d insns %llu", h.cap.substr(h.cap.size() - 9).c_str(), h.job, (unsigned long long)h.fn_insns);
        for (int r = 3; r <= 10; r++) std::printf(" %05x", h.before.regs.gpr[r].u32[3]);
        uint32_t ctx = h.before.regs.gpr[ctxreg].u32[3] & 0x3fff0;
        std::printf(" cfg ");
        for (int i = 0; i < 16; i++) std::printf("%02x", h.before.ls[ctx + i]);
        std::printf(" r3out %08x\n", h.after.regs.gpr[3].u32[3]);
        return true;
    }, edge_loaded);
    return 0;
}


}

std::map<uint32_t, NativeFn>& natives() { return natives_store(); }

std::map<std::string, CommandFn>& commands() {
    static std::map<std::string, CommandFn> m;
    return m;
}

namespace harness {
bool edge_loaded(const uint8_t* ls) { return ::edge_loaded(ls); }
std::vector<std::string> cap_list(const std::vector<std::string>& args, size_t from) { return ::cap_list(args, from); }
const std::vector<uint8_t>& elf() { return g_elf; }
}

int main(int argc, char** argv) {
    _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x6000u | 0x8040u);
    std::string root = GOWA_ROOT;
    g_elf = read_file(root + "/bin/01.12/GOWA.patched.elf");
    if (g_elf.size() < kImageFileOffset + kImageSize) {
        std::fprintf(stderr, "cannot read elf\n");
        return 2;
    }
    g_image.assign(g_elf.begin() + kImageFileOffset, g_elf.begin() + kImageFileOffset + kImageSize);
    load_callbacks();
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string cmd = args.empty() ? "replay" : args[0];
    if (cmd == "replay") return cmd_replay(args);
    if (cmd == "calls") return cmd_calls(args);
    if (cmd == "trace") return cmd_trace(args);
    if (cmd == "dump") return cmd_dump(args);
    if (cmd == "unit") return cmd_unit(args);
    if (cmd == "itrace") return cmd_itrace(args);
    if (cmd == "e2e") return cmd_e2e(args);
    if (cmd == "args") return cmd_args(args);
    if (commands().count(cmd)) return commands()[cmd](args);
    std::fprintf(stderr, "unknown command\n");
    return 2;
}
