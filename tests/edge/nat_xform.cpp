#include "natreg.h"

#include "edge_xform.h"


#include "unit.h"

#include <cstdio>
#include <string>

namespace {

int cmd_stackcheck(const std::vector<std::string>& args) {
    if (args.size() < 2) return 2;
    uint32_t fn = std::stoul(args[1], nullptr, 16);
    NativeFn nat = natives()[fn];
    int hits = 0, bad = 0;
    edgeunit::for_each_call(harness::cap_list(args, 2), fn, [&](edgeunit::Hit& h) {
        hits++;
        edgeunit::Snapshot ns = h.before;
        ns.ls.resize(spu::LS_SIZE + 16);
        ns.regs.ls = ns.ls.data();
        edge::Outputs outs;
        nat(ns.ls.data(), ns.regs, outs);
        uint32_t first = 0, cnt = 0;
        for (uint32_t i = 0; i < spu::LS_SIZE; i++)
            if (ns.ls[i] != h.after.ls[i]) {
                if (!cnt) first = i;
                cnt++;
            }
        if (cnt) {
            bad++;
            if (bad <= 5) std::printf("  job %d sp %05x diff %u first %05x\n", h.job, h.sp, cnt, first);
        }
        return true;
    }, harness::edge_loaded);
    std::printf("fn %05x hits %d full-ls-with-stack mismatches %d\n", fn, hits, bad);
    return 0;
}

CommandReg reg_stackcheck("stackcheck", cmd_stackcheck);

}

#include "capture.h"

namespace {

int cmd_endcheck(const std::vector<std::string>& args) {
    uint32_t fn = std::stoul(args[1], nullptr, 16);
    for (const std::string& p : harness::cap_list(args, 2)) {
        edgecap::Capture cap;
        if (!cap.load(p)) continue;
        edgecap::Machine m(cap);
        uint64_t last = 0;
        uint32_t ret = 0, sp = 0;
        bool open = false;
        while (!m.done()) {
            if (m.c.pc == fn && harness::edge_loaded(m.c.ls)) {
                last = m.n;
                ret = m.c.gpr[0].u32[3];
                sp = m.c.gpr[1].u32[3];
                open = true;
            }
            m.step();
            if (open && m.c.pc == ret && m.c.gpr[1].u32[3] == sp) open = false;
        }
        std::printf("%s: insns %u last call at %llu open-at-end %d\n", p.c_str(), cap.insns, (unsigned long long)last, open ? 1 : 0);
    }
    return 0;
}

CommandReg reg_endcheck("endcheck", cmd_endcheck);

}

#include <chrono>

namespace {

int cmd_xbench(const std::vector<std::string>& args) {
    uint32_t fn = std::stoul(args[1], nullptr, 16);
    int maxhits = args.size() > 2 ? std::stoi(args[2]) : 200;
    NativeFn nat = natives()[fn];
    int hits = 0;
    double total = 0;
    uint64_t insns = 0;
    edgeunit::for_each_call(harness::cap_list(args, 3), fn, [&](edgeunit::Hit& h) {
        edgeunit::Snapshot ns = h.before;
        ns.ls.resize(spu::LS_SIZE + 16);
        const int reps = 50;
        double best = 1e9;
        for (int k = 0; k < reps; k++) {
            spu::Context c = h.before.regs;
            c.ls = ns.ls.data();
            edge::Outputs outs;
            auto t0 = std::chrono::steady_clock::now();
            nat(ns.ls.data(), c, outs);
            double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (dt < best) best = dt;
        }
        total += best;
        insns += h.fn_insns;
        return ++hits < maxhits;
    }, harness::edge_loaded);
    std::printf("fn %05x hits %d spu-insns/call %.0f native best %.3f us/call\n", fn, hits, double(insns) / hits, total * 1e6 / hits);
    return 0;
}

CommandReg reg_xbench("xbench", cmd_xbench);

}
