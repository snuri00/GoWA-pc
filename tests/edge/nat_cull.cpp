#include "edge_cull.h"
#include "edge_cull_gen.h"
#include "natreg.h"

#include <cstdlib>
#include <cstring>


#include "unit.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace {

int cmd_cunit(const std::vector<std::string>& args) {
    if (args.size() < 2) return 2;
    uint32_t fn = std::stoul(args[1], nullptr, 16);
    int maxhits = args.size() > 2 ? std::stoi(args[2]) : 1000000;
    uint32_t window = std::getenv("CULL_WINDOW") ? std::stoul(std::getenv("CULL_WINDOW"), nullptr, 16) : 0;
    NativeFn nat = natives().count(fn) ? natives()[fn] : nullptr;
    if (!nat) return 2;
    int hits = 0, bad = 0;
    uint64_t insns = 0;
    double sec = 0;
    edgeunit::for_each_call(harness::cap_list(args, 3), fn, [&](edgeunit::Hit& h) {
        hits++;
        insns += h.fn_insns;
        edgeunit::Snapshot ns = h.before;
        ns.ls.resize(spu::LS_SIZE + 16);
        ns.regs.ls = ns.ls.data();
        edge::Outputs outs;
        auto t0 = std::chrono::steady_clock::now();
        bool ok = nat(ns.ls.data(), ns.regs, outs);
        sec += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        edgeunit::LsDiff d = edgeunit::diff_ls(ns.ls.data(), h.after.ls.data(), h.sp, window);
        bool r3 = std::memcmp(&ns.regs.gpr[3], &h.after.regs.gpr[3], 16) == 0;
        if (!ok || d.bytes || !r3) {
            bad++;
            if (bad <= 10) std::printf("  mismatch %s job %d: %s ls %zu [%05x..%05x] r3 %s\n", h.cap.c_str(), h.job, ok ? "ran" : "declined", d.bytes, d.first, d.last, r3 ? "ok" : "DIFF");
            if (bad <= 3 && std::getenv("CULL_SHOW")) {
                for (uint32_t a = d.first & ~15u; a <= d.last && a < (d.first & ~15u) + 0x80; a += 16) {
                    std::printf("    %05x before ", a);
                    for (int i = 0; i < 16; i++) std::printf("%02x", h.before.ls[a + i]);
                    std::printf(" want ");
                    for (int i = 0; i < 16; i++) std::printf("%02x", h.after.ls[a + i]);
                    std::printf(" got ");
                    for (int i = 0; i < 16; i++) std::printf("%02x", ns.ls[a + i]);
                    std::printf("\n");
                }
            }
        }
        return hits < maxhits;
    }, harness::edge_loaded);
    std::printf("cunit %05x: hits %d bad %d spu-insns %llu (%.0f/call) native %.3f ms (%.2f us/call, %.3f ns/spu-insn)\n", fn, hits, bad, (unsigned long long)insns, hits ? double(insns) / hits : 0.0, sec * 1e3, hits ? sec * 1e6 / hits : 0.0, insns ? sec * 1e9 / insns : 0.0);
    return bad ? 1 : 0;
}

CommandReg cmd_reg("cunit", cmd_cunit);

}

#include <windows.h>

namespace {

int cmd_cbench(const std::vector<std::string>& args) {
    if (args.size() < 2) return 2;
    uint32_t fn = std::stoul(args[1], nullptr, 16);
    int maxhits = args.size() > 2 ? std::stoi(args[2]) : 40;
    NativeFn nat = natives()[fn];
    uint64_t best_sum = 0, insns = 0;
    int hits = 0;
    std::vector<uint8_t> work(spu::LS_SIZE + 16);
    edgeunit::for_each_call(harness::cap_list(args, 3), fn, [&](edgeunit::Hit& h) {
        uint64_t best = ~0ull;
        for (int k = 0; k < 15; k++) {
            std::memcpy(work.data(), h.before.ls.data(), spu::LS_SIZE);
            spu::Context c = h.before.regs;
            c.ls = work.data();
            edge::Outputs outs;
            ULONG64 t0 = 0, t1 = 0;
            QueryThreadCycleTime(GetCurrentThread(), &t0);
            nat(work.data(), c, outs);
            QueryThreadCycleTime(GetCurrentThread(), &t1);
            best = std::min<uint64_t>(best, t1 - t0);
        }
        best_sum += best;
        insns += h.fn_insns;
        return ++hits < maxhits;
    }, harness::edge_loaded);
    std::printf("cbench %05x: hits %d cycles/call %.0f cycles/spu-insn %.3f\n", fn, hits, double(best_sum) / hits, double(best_sum) / insns);
    return 0;
}

CommandReg cmd_reg2("cbench", cmd_cbench);

}
