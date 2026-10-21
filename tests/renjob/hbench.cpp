#include "capture.h"
#include "renjob_gen.h"

#include <intrin.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Root {
    uint32_t lsa;
    void (RJ_ABI *fn)(renjob::xo::V*, uint8_t*, renjob::xo::Ctl&);
    const char* name;
};

#define RJ_HB(addr, f, j) {0x##addr, renjob::xo::f, #f},
const Root kRoots[] = {RJ_ROOTS(RJ_HB)};
#undef RJ_HB

}

int rj_hbench(const std::vector<uint8_t>& image, const std::vector<std::string>& args, const std::vector<std::string>& caps) {
    uint32_t only = args.size() > 1 && args[1] != "all" ? std::stoul(args[1], nullptr, 16) : 0;
    int maxhits = args.size() > 2 ? std::stoi(args[2]) : 50;
    int iters = 50;
    for (const Root& r : kRoots) {
        if (only && r.lsa != only) continue;
        uint64_t best_sum = 0, med_sum = 0;
        int hits = 0;
        for (const std::string& path : caps) {
            edgecap::Capture cap;
            if (!cap.load(path)) continue;
            edgecap::Machine m(cap);
            while (!m.done() && hits < maxhits) {
                if (m.c.pc == r.lsa && std::memcmp(m.c.ls + 0x4000, image.data(), 0x30) == 0) {
                    std::vector<uint8_t> ls(m.ls.begin(), m.ls.end());
                    alignas(16) renjob::xo::V R[128], R0[128];
                    std::memcpy(R0, m.c.gpr, sizeof R0);
                    renjob::xo::Ctl ctl;
                    renjob::xo::JournalStore js;
                    js.attach(ctl);
                    std::vector<uint64_t> t;
                    bool ok = true;
                    std::memcpy(R, R0, sizeof R);
                    ctl.reset();
                    r.fn(R, ls.data(), ctl);
                    if (ctl.fail) ok = false;
                    std::vector<uint32_t> saved_a;
                    std::vector<uint8_t> saved_v;
                    for (uint32_t k = 0; k < ctl.jn; k++) {
                        saved_a.push_back(ctl.j[k].a);
                        const uint8_t* p = reinterpret_cast<const uint8_t*>(&ctl.j[k].old);
                        saved_v.insert(saved_v.end(), p, p + 16);
                    }
                    ctl.rollback(ls.data());
                    renjob::xo::Ctl plain;
                    for (int k = 0; k < iters && ok; k++) {
                        std::memcpy(R, R0, sizeof R);
                        plain.out = false;
                        uint64_t t0 = __rdtsc();
                        r.fn(R, ls.data(), plain);
                        uint64_t t1 = __rdtsc();
                        t.push_back(t1 - t0);
                        for (size_t q = 0; q < saved_a.size(); q++) std::memcpy(ls.data() + saved_a[q], saved_v.data() + 16 * q, 16);
                    }
                    if (ok && !t.empty()) {
                        std::sort(t.begin(), t.end());
                        best_sum += t[0];
                        med_sum += t[t.size() / 2];
                        hits++;
                    }
                }
                m.step();
            }
            if (hits >= maxhits) break;
        }
        if (hits) std::printf("%-10s hits %3d best %7.0f median %7.0f TSC cycles per call (warm, generated code only)\n", r.name, hits, double(best_sum) / hits, double(med_sum) / hits);
    }
    return 0;
}
