#include "capture.h"
#include "images.h"
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

int sc_main_ext(const std::string& cmd, const std::vector<std::string>& args, int& rc);

namespace {

std::string cap_dir() {
    const char* d = std::getenv("SC_CAPDIR");
    return d ? std::string(d) : std::string(GOWA_ROOT) + "/out/spucap_scream";
}

std::string cap_path(const std::string& s) {
    if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos) return s;
    return cap_dir() + "/" + s + (s.find(".bin") == std::string::npos ? ".bin" : "");
}

}

std::vector<std::string> sc_cap_list(const std::vector<std::string>& args, size_t from) {
    std::vector<std::string> v;
    for (size_t i = from; i < args.size(); i++) v.push_back(cap_path(args[i]));
    if (v.empty())
        for (int i = 0; i < 64; i++) {
            std::string p = cap_path("cap_" + std::to_string(i));
            if (std::ifstream(p).good()) v.push_back(p);
        }
    return v;
}

namespace {

bool is_call(const spudec::Insn& in, uint32_t pc, uint32_t npc, const spu::Context& c) {
    return (spudec::is(in.name, "brsl") || spudec::is(in.name, "brasl") || spudec::is(in.name, "bisl")) && npc != pc + 4 && in.dst >= 0 && c.gpr[in.dst].u32[3] == pc + 4;
}

int cmd_scan(const std::vector<std::string>& args) {
    for (const std::string& path : sc_cap_list(args, 1)) {
        edgecap::Capture cap;
        if (!cap.load(path)) {
            std::printf("%s: bad capture\n", path.c_str());
            continue;
        }
        scimg::TrackedMachine m(cap);
        std::map<std::pair<int, uint32_t>, uint64_t> per;
        uint64_t kernel = 0, other = 0;
        std::map<uint32_t, uint64_t> other_pg;
        const bool want_pages = std::getenv("SC_OTHER") != nullptr;
        while (!m.done()) {
            uint32_t pc = m.c.pc;
            uint32_t base = 0;
            int img = m.tr.find(m.c.ls, pc, &base);
            if (img >= 0) per[{img, base}]++;
            else if (pc < 0x4000) kernel++;
            else {
                other++;
                if (want_pages) other_pg[pc >> 12]++;
            }
            m.tstep();
        }
        std::string diff;
        bool ok = m.verify_final(diff);
        uint64_t tot = 0;
        for (auto& [k, v] : per) tot += v;
        std::printf("%s: insns %llu scream %llu (%.1f%%) kernel %llu other %llu %s%s\n", path.c_str(), (unsigned long long)m.n, (unsigned long long)tot, 100.0 * tot / std::max<uint64_t>(1, m.n), (unsigned long long)kernel, (unsigned long long)other, ok ? "exact" : "DIFF", diff.c_str());
        for (auto& [k, v] : per) std::printf("    %-14s base %05x insns %llu\n", scimg::images()[k.first].name, k.second, (unsigned long long)v);
        for (auto& [k, v] : other_pg) std::printf("    other page %05x insns %llu\n", k << 12, (unsigned long long)v);
    }
    return 0;
}

int cmd_hist(const std::vector<std::string>& args) {
    std::map<std::pair<int, uint32_t>, uint64_t> counts;
    for (const std::string& path : sc_cap_list(args, 1)) {
        edgecap::Capture cap;
        if (!cap.load(path)) continue;
        scimg::TrackedMachine m(cap);
        while (!m.done()) {
            uint32_t pc = m.c.pc, base = 0;
            int img = m.tr.find(m.c.ls, pc, &base);
            if (img >= 0) counts[{img, pc - base}]++;
            m.tstep();
        }
    }
    for (auto& [k, n] : counts) std::printf("%s %05x %llu\n", scimg::images()[k.first].name, k.second, (unsigned long long)n);
    return 0;
}

struct FnStat {
    uint64_t calls = 0;
    uint64_t incl = 0;
    uint64_t excl = 0;
    uint64_t impure = 0;
    std::set<uint32_t> callers;
    std::set<uint32_t> bases;
};

int cmd_calls(const std::vector<std::string>& args) {
    std::map<std::pair<int, uint32_t>, FnStat> stats;
    std::map<int, uint64_t> total;
    std::map<std::string, uint64_t> chan;
    std::map<std::pair<std::pair<int, uint32_t>, std::pair<int, uint32_t>>, std::pair<uint64_t, uint64_t>> edges;
    for (const std::string& path : sc_cap_list(args, 1)) {
        edgecap::Capture cap;
        if (!cap.load(path)) continue;
        scimg::TrackedMachine m(cap);
        struct Frame {
            int img;
            uint32_t fn, ret, base;
            uint64_t start, child;
            bool impure;
        };
        std::vector<Frame> st;
        while (!m.done()) {
            uint32_t pc = m.c.pc, base = 0;
            int img = m.tr.find(m.c.ls, pc, &base);
            uint32_t w = scimg::be32(m.c.ls + pc);
            uint32_t op11 = w >> 21;
            bool chan_op = op11 == 0x00d || op11 == 0x10d || op11 == 0x00f || op11 == 0x000 || op11 == 0x140;
            bool call_op = (w >> 23) == 0x66 || (w >> 23) == 0x62 || op11 == 0x1a9;
            size_t p0 = m.pos;
            m.tstep();
            if (img >= 0) total[img]++;
            uint32_t npc = m.c.pc;
            if (npc == pc + 4 && !chan_op) continue;
            spudec::Insn in = spudec::decode(w);
            if (m.pos != p0 && chan_op) {
                for (Frame& f : st) f.impure = true;
                if (img >= 0) {
                    char b[96];
                    std::snprintf(b, sizeof b, "%s %05x %s ch%u", scimg::images()[img].name, pc - base, in.name, (w >> 7) & 0x7f);
                    chan[b]++;
                }
            }
            if (call_op && is_call(in, pc, npc, m.c)) {
                uint32_t nb = 0;
                int ni = m.tr.find(m.c.ls, npc, &nb);
                st.push_back({ni, ni >= 0 ? npc - nb : npc, pc + 4, nb, m.n, 0, false});
                if (st.size() > 256) st.erase(st.begin());
                auto& s = stats[{ni, ni >= 0 ? npc - nb : npc}];
                s.callers.insert(img >= 0 ? pc - base : pc | 0x80000000u);
                s.bases.insert(nb);
            } else if (npc != pc + 4) {
                for (size_t k = st.size(); k-- > 0;) {
                    if (st[k].ret == npc) {
                        while (st.size() > k) {
                            Frame f = st.back();
                            st.pop_back();
                            uint64_t incl = m.n - f.start;
                            FnStat& s = stats[{f.img, f.fn}];
                            s.calls++;
                            s.incl += incl;
                            s.excl += incl - f.child;
                            s.impure += f.impure;
                            if (!st.empty()) {
                                st.back().child += incl;
                                auto& e = edges[{{st.back().img, st.back().fn}, {f.img, f.fn}}];
                                e.first++;
                                e.second += incl;
                            }
                        }
                        break;
                    }
                }
            }
        }
    }
    for (auto& [i, t] : total) std::printf("image %s insns %llu\n", scimg::images()[i].name, (unsigned long long)t);
    std::vector<std::pair<std::pair<int, uint32_t>, FnStat>> v(stats.begin(), stats.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.incl > b.second.incl; });
    for (auto& [k, s] : v) {
        if (!s.calls) continue;
        std::printf("fn %-13s %05x calls %7llu incl %10llu excl %10llu per-call %8.1f impure %6llu bases", k.first >= 0 ? scimg::images()[k.first].name : "-", k.second, (unsigned long long)s.calls, (unsigned long long)s.incl, (unsigned long long)s.excl,
                    double(s.incl) / std::max<uint64_t>(1, s.calls), (unsigned long long)s.impure);
        for (uint32_t b : s.bases) std::printf(" %05x", b);
        std::printf(" callers");
        int n = 0;
        for (uint32_t c : s.callers) {
            if (n++ > 5) break;
            std::printf(" %05x", c);
        }
        std::printf("\n");
    }
    for (auto& [k, n] : chan) std::printf("chan %s x%llu\n", k.c_str(), (unsigned long long)n);
    for (auto& [k, e] : edges)
        if (k.first.first >= 0 && e.second >= 100000) std::printf("edge %s %05x -> %05x calls %llu incl %llu\n", scimg::images()[k.first.first].name, k.first.second, k.second.second, (unsigned long long)e.first, (unsigned long long)e.second);
    return 0;
}

}

int main(int argc, char** argv) {
    _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x6000u | 0x8040u);
    if (!scimg::load_images(std::string(GOWA_ROOT) + "/bin/01.12/GOWA.patched.elf")) {
        std::fprintf(stderr, "cannot read elf\n");
        return 2;
    }
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string cmd = args.empty() ? "scan" : args[0];
    if (cmd == "scan") return cmd_scan(args);
    if (cmd == "hist") return cmd_hist(args);
    if (cmd == "calls") return cmd_calls(args);
    int rc = 2;
    if (sc_main_ext(cmd, args, rc)) return rc;
    std::fprintf(stderr, "unknown command\n");
    return 2;
}
