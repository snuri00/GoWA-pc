#include "edge_softbody.h"
#include "natreg.h"
#include "unit.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <immintrin.h>

namespace {

int cmd_sbbench(const std::vector<std::string>& args) {
    int maxhits = args.size() > 1 ? std::stoi(args[1]) : 1000000;
    int rounds = args.size() > 2 ? std::stoi(args[2]) : 5;
    struct Case {
        std::vector<uint8_t> ls;
        spu::Context regs;
        uint64_t insns;
    };
    std::vector<Case> cases;
    edgeunit::for_each_call(harness::cap_list(args, 3), 0x73e0, [&](edgeunit::Hit& h) {
        Case c;
        c.ls = h.before.ls;
        c.ls.resize(0x40010);
        c.regs = h.before.regs;
        c.insns = h.fn_insns;
        cases.push_back(std::move(c));
        return int(cases.size()) < maxhits;
    }, harness::edge_loaded);
    std::vector<uint8_t> work(0x40010);
    NativeFn nat = natives()[0x73e0];
    double best_tsc = 1e300, best_us = 1e300;
    uint64_t insns = 0;
    for (auto& c : cases) insns += c.insns;
    for (int r = 0; r < rounds; r++) {
        uint64_t tsc = 0;
        double us = 0;
        for (Case& c : cases) {
            std::memcpy(work.data(), c.ls.data(), work.size());
            spu::Context ctx = c.regs;
            ctx.ls = work.data();
            edge::Outputs outs;
            auto w0 = std::chrono::steady_clock::now();
            uint64_t t0 = __rdtsc();
            nat(work.data(), ctx, outs);
            tsc += __rdtsc() - t0;
            us += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - w0).count();
        }
        best_tsc = std::min(best_tsc, double(tsc));
        best_us = std::min(best_us, us);
    }
    uint64_t f = 0, c = 0, e = 0, l = 0, ld = 0;
    edge::soft_body_stats(&f, &c, &e, &l, &ld);
    std::printf("73e0: %zu calls, %.0f TSC cycles/call (%.2f us), %.3f cycles per SPU insn (%.0f SPU insns/call), groups fast %llu checked %llu exact %llu, loops native %llu per-group %llu\n", cases.size(), best_tsc / cases.size(), best_us / cases.size(), best_tsc / insns, double(insns) / cases.size(), (unsigned long long)f, (unsigned long long)c, (unsigned long long)e, (unsigned long long)l, (unsigned long long)ld);
    return 0;
}

CommandReg reg_sbbench("sbbench", cmd_sbbench);

uint32_t r32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

void w32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

uint32_t special(std::mt19937& g, uint32_t v) {
    switch (g() % 14) {
    case 0: return 0;
    case 1: return 0x80000000u;
    case 2: return (g() & 0x80000000u) | 0x7f800000u | (g() & 0x7fffffu);
    case 3: return g() & 0x807fffffu;
    case 4: return 0x7f000000u | (g() & 0x80ffffffu);
    case 5: return v ^ 0x80000000u;
    case 6: return v + (g() % 5) - 2;
    case 7: return (g() & 0x80000000u) | (0x4d000000u + (g() % 0x02000000u));
    case 8: return (g() & 0x80000000u) | (0x5e000000u + (g() % 0x10000000u));
    case 9: return (g() & 0x80000000u) | (0x00800000u + (g() % 0x01000000u));
    case 10: return (g() & 0x80000000u) | 0x3f800000u;
    case 11: return (g() & 0x80000000u) | (0x2a000000u + (g() % 0x08000000u));
    default: {
        float f;
        std::memcpy(&f, &v, 4);
        f *= std::uniform_real_distribution<float>(-3.0f, 3.0f)(g);
        std::memcpy(&v, &f, 4);
        return v;
    }
    }
}

int cmd_sbfuzz(const std::vector<std::string>& args) {
    int count = args.size() > 1 ? std::stoi(args[1]) : 500;
    uint32_t seed = args.size() > 2 ? uint32_t(std::stoul(args[2])) : 1;
    std::vector<edgeunit::Hit> base;
    int seen = 0;
    edgeunit::for_each_call(harness::cap_list(args, 3), 0x73e0, [&](edgeunit::Hit& h) {
        if (seen++ % 5 == 0) base.push_back(h);
        return base.size() < 120;
    }, harness::edge_loaded);
    if (base.empty()) return 2;
    std::mt19937 g(seed);
    NativeFn nat = natives()[0x73e0];
    int bad = 0, ran = 0, skipped = 0;
    uint64_t f0 = 0, c0 = 0, e0 = 0, l0 = 0, d0 = 0;
    edge::soft_body_stats(&f0, &c0, &e0, &l0, &d0);
    std::map<int, int> modes;
    for (int it = 0; it < count; it++) {
        const edgeunit::Hit& h = base[g() % base.size()];
        edgeunit::Snapshot s = h.before;
        s.ls.resize(spu::LS_SIZE + 16);
        uint8_t* ls = s.ls.data();
        auto& gpr = s.regs.gpr;
        uint32_t ctx = gpr[3].u32[3] & 0x3fff0, pos = gpr[9].u32[3], nrm = gpr[10].u32[3], nv = gpr[11].u32[3], blk = gpr[12].u32[3] & 0x3fff0;
        uint32_t off0 = r32(ls + blk), off1 = r32(ls + blk + 4), off2 = r32(ls + blk + 8);
        uint32_t n1 = r32(ls + blk + 16), n2 = r32(ls + blk + 20), n3 = r32(ls + blk + 24);
        uint32_t total = n1 + n2 + n3;
        int mode = int(g() % 8);
        modes[mode]++;
        auto poke = [&](uint32_t a, int rate) {
            if (g() % rate == 0) w32(ls + (a & 0x3fffc), special(g, r32(ls + (a & 0x3fffc))));
        };
        if (mode == 0 || mode == 7)
            for (uint32_t v = 0; v < nv && v < 4096; v++)
                for (int c = 0; c < 4; c++) poke(pos + 16 * v + 4 * c, 60);
        if (mode == 1 || mode == 7)
            for (uint32_t v = 0; v < nv && v < 4096; v++)
                for (int c = 0; c < 3; c++) poke(nrm + 16 * v + 4 * c, 60);
        if (mode == 2 || mode == 7)
            for (uint32_t k = 0; k < total && k < 256; k++)
                for (int c = 0; c < 4; c++) {
                    poke(blk + off0 + 16 * k + 4 * c, 12);
                    poke(blk + off1 + 16 * k + 4 * c, 12);
                    poke(blk + off2 + 16 * k + 4 * c, 12);
                }
        if (mode == 3) {
            uint32_t sc = g() % 3 ? special(g, gpr[14].u32[3]) : gpr[14].u32[3];
            for (int i = 0; i < 4; i++) gpr[14].u32[i] = sc;
            if (g() % 4 == 0) gpr[14].u32[g() % 3] ^= 1;
        }
        if (mode == 4) {
            uint32_t idx = r32(ls + ctx + 80);
            uint32_t ni = (uint32_t(ls[ctx + 400]) << 8) | ls[ctx + 401];
            for (uint32_t i = 0; i < ni; i++)
                if (g() % 30 == 0) {
                    uint32_t v = g() % (nv ? nv : 1);
                    ls[(idx + 2 * i) & 0x3ffff] = uint8_t(v >> 8);
                    ls[(idx + 2 * i + 1) & 0x3ffff] = uint8_t(v);
                }
        }
        if (mode == 5) {
            uint32_t a = g() % (total + 1), b = g() % (total - a + 1), c = total - a - b;
            if (g() % 2) c = g() % (c + 1);
            w32(ls + blk + 16, a);
            w32(ls + blk + 20, b);
            w32(ls + blk + 24, c);
            if (g() % 3 == 0) gpr[13].u32[3] ^= 256;
        }
        if (mode == 6) {
            uint32_t bits[] = {256, 0x800, 0x10000, 1, 2, 4, 0x100000};
            gpr[13].u32[3] ^= bits[g() % 7];
            if (g() % 4 == 0) gpr[8].u32[3] = g() % 2 ? 0 : blk;
        }
        edgeunit::Snapshot iso = s;
        edgeunit::IsoResult r = edgeunit::run_isolated(iso, h.ret, h.sp);
        if (!r.ok) {
            skipped++;
            continue;
        }
        edgeunit::Snapshot ns = s;
        ns.ls.resize(spu::LS_SIZE + 16);
        ns.regs.ls = ns.ls.data();
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
    uint64_t f1 = 0, c1 = 0, e1 = 0, l1 = 0, d1 = 0;
    edge::soft_body_stats(&f1, &c1, &e1, &l1, &d1);
    std::printf("sbfuzz: %d cases ran, %d skipped (SPU run failed), %d mismatching; groups fast %llu checked %llu exact %llu, loops native %llu per-group %llu; modes", ran, skipped, bad, (unsigned long long)(f1 - f0), (unsigned long long)(c1 - c0), (unsigned long long)(e1 - e0), (unsigned long long)(l1 - l0), (unsigned long long)(d1 - d0));
    for (auto& [m, n] : modes) std::printf(" %d:%d", m, n);
    std::printf("\n");
    return bad ? 1 : 0;
}

CommandReg reg_sbfuzz("sbfuzz", cmd_sbfuzz);

}
