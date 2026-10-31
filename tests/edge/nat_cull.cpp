#include "edge_cull.h"
#include "edge_cull_gen.h"
#include "natreg.h"



#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>


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

#include <x86intrin.h>

namespace {

int cmd_cbench(const std::vector<std::string>& args) {
    if (args.size() < 2) return 2;
    uint32_t fn = std::stoul(args[1], nullptr, 16);
    int maxhits = args.size() > 2 ? std::stoi(args[2]) : 40;
    NativeFn nat = natives()[fn];
    bool warm = std::getenv("CBENCH_WARM") != nullptr;
    uint32_t warm_lo = warm ? uint32_t(std::stoul(std::getenv("CBENCH_WARM"), nullptr, 16)) : 0;
    uint64_t best_sum = 0, insns = 0;
    int hits = 0;
    std::vector<uint8_t> work(spu::LS_SIZE + 16);
    edgeunit::for_each_call(harness::cap_list(args, 3), fn, [&](edgeunit::Hit& h) {
        uint64_t best = ~0ull;
        for (int k = 0; k < 15; k++) {
            std::memcpy(work.data(), h.before.ls.data(), spu::LS_SIZE);
            if (warm) {
                volatile uint64_t acc = 0;
                uint64_t a2 = 0;
                for (uint32_t a = warm_lo; a < spu::LS_SIZE; a += 64) a2 += work[a];
                acc = a2;
                (void)acc;
            }
            spu::Context c = h.before.regs;
            c.ls = work.data();
            edge::Outputs outs;
            uint64_t t0 = __rdtsc();
            nat(work.data(), c, outs);
            uint64_t t1 = __rdtsc();
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

namespace {

int cmd_cstats(const std::vector<std::string>& args) {
    std::map<std::string, int> hist;
    int hits = 0, fallback = 0;
    edgeunit::for_each_call(harness::cap_list(args, 1), 0xd618, [&](edgeunit::Hit& h) {
        hits++;
        const uint8_t* ls = h.before.ls.data();
        uint32_t ctx = h.before.regs.gpr[3].u32[3] & 0x3fff0;
        char b[200];
        std::snprintf(b, sizeof b, "r4 %x r5 %x cfg0 %02x idxfl %x smp %02x cb %08x", h.before.regs.gpr[4].u32[3], h.before.regs.gpr[5].u32[3], ls[ctx], ls[ctx + 6] >> 4, ls[ctx + 320], (uint32_t(ls[ctx + 388]) << 24) | (uint32_t(ls[ctx + 389]) << 16) | (uint32_t(ls[ctx + 390]) << 8) | ls[ctx + 391]);
        hist[b]++;
        std::vector<uint8_t> w = h.before.ls;
        w.resize(spu::LS_SIZE + 16);
        edge::CullIn in;
        in.ctx = ctx;
        auto be = [&](uint8_t* o, int r) {
            for (int j = 0; j < 4; j++) {
                uint32_t v = h.before.regs.gpr[r].u32[3 - j];
                o[4 * j] = uint8_t(v >> 24);
                o[4 * j + 1] = uint8_t(v >> 16);
                o[4 * j + 2] = uint8_t(v >> 8);
                o[4 * j + 3] = uint8_t(v);
            }
        };
        be(in.r0, 0);
        be(in.r1, 1);
        be(in.r4, 4);
        be(in.r5, 5);
        be(in.r80, 80);
        be(in.r81, 81);
        be(in.r82, 82);
        be(in.r126, 126);
        if (edge::cull_triangles(w.data(), in) < 0) fallback++;
        return true;
    }, harness::edge_loaded);
    for (auto& [k, n] : hist) std::printf("  %6d  %s\n", n, k.c_str());
    std::printf("cstats: hits %d native-declined %d\n", hits, fallback);
    return 0;
}

CommandReg cmd_reg3("cstats", cmd_cstats);

}

#include <random>

namespace {

int cmd_cfuzz(const std::vector<std::string>& args) {
    int count = args.size() > 1 ? std::stoi(args[1]) : 500;
    uint32_t seed = args.size() > 2 ? uint32_t(std::stoul(args[2])) : 1;
    std::vector<edgeunit::Hit> base;
    int seen = 0;
    edgeunit::for_each_call(harness::cap_list(args, 3), 0xd618, [&](edgeunit::Hit& h) {
        if (seen++ % 7 == 0) base.push_back(h);
        return base.size() < 60;
    }, harness::edge_loaded);
    if (base.empty()) return 2;
    std::mt19937 g(seed);
    NativeFn nat = natives()[0xd618];
    int bad = 0, ran = 0, declined = 0;
    auto w32 = [](uint8_t* p, uint32_t v) {
        p[0] = uint8_t(v >> 24);
        p[1] = uint8_t(v >> 16);
        p[2] = uint8_t(v >> 8);
        p[3] = uint8_t(v);
    };
    auto r32 = [](const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; };
    auto special = [&](uint32_t v) -> uint32_t {
        switch (g() % 10) {
        case 0: return 0;
        case 1: return 0x80000000u;
        case 2: return 0x7f800000u | (g() & 0x807fffffu);
        case 3: return g() & 0x807fffffu;
        case 4: return 0x7f000000u | (g() & 0x80ffffffu);
        case 5: return v ^ 0x80000000u;
        case 6: return v + (g() % 5) - 2;
        default: return v;
        }
    };
    for (int it = 0; it < count; it++) {
        const edgeunit::Hit& h = base[g() % base.size()];
        edgeunit::Snapshot s = h.before;
        s.ls.resize(spu::LS_SIZE + 16);
        uint8_t* ls = s.ls.data();
        uint32_t ctx = s.regs.gpr[3].u32[3] & 0x3fff0;
        int mode = int(g() % 6);
        uint32_t nv = (uint32_t(ls[ctx + 8]) << 8) | ls[ctx + 9];
        uint32_t pos = r32(ls + ctx + 92);
        if (mode == 0 || mode == 5) {
            for (uint32_t v = 0; v < nv; v++)
                for (int c = 0; c < 3; c++)
                    if (g() % 50 == 0) w32(ls + ((pos + 16 * v + 4 * c) & 0x3fffc), special(r32(ls + ((pos + 16 * v + 4 * c) & 0x3fffc))));
        }
        if (mode == 1 || mode == 5) {
            for (int i = 0; i < 16; i++)
                if (g() % 4 == 0) {
                    float f;
                    uint32_t b = r32(ls + ctx + 224 + 4 * i);
                    std::memcpy(&f, &b, 4);
                    f *= 0.5f + float(g() % 1000) / 500.0f;
                    std::memcpy(&b, &f, 4);
                    if (g() % 8 == 0) b = special(b);
                    w32(ls + ctx + 224 + 4 * i, b);
                }
        }
        if (mode == 2 || mode == 5) {
            for (int i = 0; i < 6; i++)
                if (g() % 3 == 0) {
                    uint32_t b = r32(ls + ctx + 288 + 4 * i + (i >= 3 ? 4 : 0));
                    w32(ls + ctx + 288 + 4 * i + (i >= 3 ? 4 : 0), g() % 4 == 0 ? special(b) : (b ^ (g() % 3 == 0 ? 0x80000000u : 0)));
                }
            for (int i = 0; i < 8; i++)
                if (g() % 4 == 0) ls[ctx + 208 + i] = uint8_t(g());
            if (g() % 2) {
                uint32_t a = r32(ls + ctx + 216), b = r32(ls + ctx + 220);
                w32(ls + ctx + 216, b);
                w32(ls + ctx + 220, g() % 4 ? a : special(a));
            }
        }
        if (mode == 3 || mode == 5) {
            uint32_t idx = r32(ls + ctx + 80);
            uint32_t ni = (uint32_t(ls[ctx + 400]) << 8) | ls[ctx + 401];
            for (uint32_t i = 0; i < ni; i++)
                if (g() % 40 == 0) {
                    uint32_t v = g() % 8 == 0 ? (g() & 0xffff) : g() % (nv + 2);
                    ls[(idx + 2 * i) & 0x3ffff] = uint8_t(v >> 8);
                    ls[(idx + 2 * i + 1) & 0x3ffff] = uint8_t(v);
                }
        }
        if (mode == 4 || mode == 5) {
            uint32_t fl = g() % 8 == 0 ? g() % 6 : 1 + g() % 3;
            s.regs.gpr[4].u32[3] = fl;
            uint8_t sm[] = {0, 0, 3, 4, 1, 5};
            ls[ctx + 320] = sm[g() % 6];
            ls[ctx + 6] = uint8_t((ls[ctx + 6] & 15) | ((g() % 4) << 4));
        }
        edgeunit::Snapshot iso = s;
        edgeunit::IsoResult r = edgeunit::run_isolated(iso, h.ret, h.sp);
        if (!r.ok) continue;
        edgeunit::Snapshot ns = s;
        ns.ls.resize(spu::LS_SIZE + 16);
        ns.regs.ls = ns.ls.data();
        edge::CullIn in;
        in.ctx = ctx;
        std::vector<uint8_t> probe = s.ls;
        auto be = [&](uint8_t* o, int rr) {
            for (int j = 0; j < 4; j++) {
                uint32_t v = s.regs.gpr[rr].u32[3 - j];
                o[4 * j] = uint8_t(v >> 24);
                o[4 * j + 1] = uint8_t(v >> 16);
                o[4 * j + 2] = uint8_t(v >> 8);
                o[4 * j + 3] = uint8_t(v);
            }
        };
        be(in.r0, 0);
        be(in.r1, 1);
        be(in.r4, 4);
        be(in.r5, 5);
        be(in.r80, 80);
        be(in.r81, 81);
        be(in.r82, 82);
        be(in.r126, 126);
        if (edge::cull_triangles(probe.data(), in) < 0) declined++;
        edge::Outputs outs;
        nat(ns.ls.data(), ns.regs, outs);
        ran++;
        edgeunit::LsDiff d = edgeunit::diff_ls(ns.ls.data(), iso.ls.data(), 0, 0);
        bool r3 = std::memcmp(&ns.regs.gpr[3], &iso.regs.gpr[3], 16) == 0;
        if (d.bytes || !r3) {
            bad++;
            if (bad <= 8) std::printf("  case %d mode %d job %d: ls %zu [%05x..%05x] r3 %s\n", it, mode, h.job, d.bytes, d.first, d.last, r3 ? "ok" : "DIFF");
        }
    }
    std::printf("cfuzz: %d cases ran, %d native-declined (generated path), %d mismatching\n", ran, declined, bad);
    return bad ? 1 : 0;
}

CommandReg cmd_reg4("cfuzz", cmd_cfuzz);

}
