#include "edge_cull_ops.h"
#include "natreg.h"
#include "unit.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

uint32_t fword(std::mt19937& g) {
    uint32_t s = g() & 0x80000000u;
    switch (g() % 10) {
    case 0: return s | (g() & 0x7fffff);
    case 1: return s | 0x7f800000u | (g() & 0x7fffff);
    case 2: return s;
    case 3: return s | ((100u + g() % 80u) << 23) | (g() & 0x7fffff);
    case 4: return s | ((120u + g() % 40u) << 23) | (g() & 0x7fffff);
    case 5: return s | ((140u + g() % 50u) << 23) | (g() & 0x7fffff);
    case 6: return s | ((g() % 256u) << 23) | (g() & 0x7fffff);
    case 7: return s | ((g() % 256u) << 23) | ((g() & 1) ? 0x7fffffu : 0u);
    case 8: return g() & 0x00ffffffu;
    default: return g();
    }
}

uint32_t iword(std::mt19937& g) {
    switch (g() % 6) {
    case 0: return g() & 0xffffff;
    case 1: return uint32_t(-int32_t(g() & 0xffffff));
    case 2: return 0x1000000u - (g() & 3);
    case 3: return uint32_t(-0x1000000 + int32_t(g() & 3));
    case 4: return g() & 0xff;
    default: return g();
    }
}

int cmd_opfuzz(const std::vector<std::string>& args) {
    uint64_t n = args.size() > 1 ? std::stoull(args[1]) : 1000000;
    std::mt19937 g(args.size() > 2 ? std::stoul(args[2]) : 1);
    const int scales[] = {124, 139, 140, 142, 154, 155, 157, 158, 161, 165, 167, 168, 169, 171, 173, 0, 14, 15, 30, 200, 254};
    uint64_t bad[6] = {};
    for (uint64_t it = 0; it < n; it++) {
        alignas(16) uint32_t f[4], u[4], r[4];
        for (int k = 0; k < 4; k++) {
            f[k] = fword(g);
            u[k] = iword(g);
        }
        if (g() % 4 == 0)
            for (int k = 1; k < 4; k++) u[k] = u[0] + k;
        edge::xo::V fv = _mm_load_si128(reinterpret_cast<const __m128i*>(f));
        edge::xo::V uv = _mm_load_si128(reinterpret_cast<const __m128i*>(u));
        int i8 = scales[g() % (sizeof scales / sizeof scales[0])];
        bool ranged = i8 >= 30 && i8 <= 258;
        _mm_store_si128(reinterpret_cast<__m128i*>(r), edge::xo::cfltu(fv, i8));
        for (int k = 0; k < 4; k++)
            if (r[k] != spuf::cfltu(f[k], i8) && bad[0]++ < 8) std::printf("cfltu %08x i8 %d: %08x vs %08x\n", f[k], i8, r[k], spuf::cfltu(f[k], i8));
        _mm_store_si128(reinterpret_cast<__m128i*>(r), edge::xo::cflts_slow(fv, i8));
        for (int k = 0; k < 4; k++)
            if (r[k] != uint32_t(spuf::cflts(f[k], i8)) && bad[1]++ < 8) std::printf("cflts %08x i8 %d: %08x vs %08x\n", f[k], i8, r[k], uint32_t(spuf::cflts(f[k], i8)));
        _mm_store_si128(reinterpret_cast<__m128i*>(r), edge::xo::cflts(fv, i8));
        for (int k = 0; k < 4; k++)
            if (r[k] != uint32_t(spuf::cflts(f[k], i8)) && bad[2]++ < 8) std::printf("cflts* %08x i8 %d: %08x vs %08x\n", f[k], i8, r[k], uint32_t(spuf::cflts(f[k], i8)));
        if (ranged) {
            spuf::CsrGuard csr;
            _mm_store_si128(reinterpret_cast<__m128i*>(r), edge::xo::cuflt_slow(uv, i8));
            for (int k = 0; k < 4; k++)
                if (r[k] != spuf::cuflt(u[k], i8) && bad[3]++ < 8) std::printf("cuflt %08x i8 %d: %08x vs %08x\n", u[k], i8, r[k], spuf::cuflt(u[k], i8));
            _mm_store_si128(reinterpret_cast<__m128i*>(r), edge::xo::csflt(uv, i8));
            for (int k = 0; k < 4; k++)
                if (r[k] != spuf::csflt(int32_t(u[k]), i8) && bad[4]++ < 8) std::printf("csflt %08x i8 %d: %08x vs %08x\n", u[k], i8, r[k], spuf::csflt(int32_t(u[k]), i8));
        }
        uint32_t m = g() & 0xffff;
        alignas(16) uint8_t b[16];
        _mm_store_si128(reinterpret_cast<__m128i*>(b), edge::xo::fsmbi(m));
        for (int k = 0; k < 16; k++)
            if (b[k] != (((m >> k) & 1) ? 0xff : 0) && bad[5]++ < 8) std::printf("fsmbi %04x byte %d: %02x\n", m, k, b[k]);
    }
    std::printf("opfuzz %llu iterations: cfltu %llu cflts %llu cflts173 %llu cuflt %llu csflt %llu fsmbi %llu bad (avx2 %d)\n", (unsigned long long)n, (unsigned long long)bad[0], (unsigned long long)bad[1], (unsigned long long)bad[2],
                (unsigned long long)bad[3], (unsigned long long)bad[4], (unsigned long long)bad[5], spuf::avx2_on() ? 1 : 0);
    return (bad[0] | bad[1] | bad[2] | bad[3] | bad[4] | bad[5]) ? 1 : 0;
}

CommandReg reg_opfuzz("opfuzz", cmd_opfuzz);

uint32_t r32b(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

void w32b(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

uint32_t specialf(std::mt19937& g, uint32_t v) {
    switch (g() % 15) {
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
    case 12: return (g() & 0x80000000u) | 0x7fffffffu;
    case 13: return g();
    default: {
        float f;
        std::memcpy(&f, &v, 4);
        f *= std::uniform_real_distribution<float>(-3.0f, 3.0f)(g);
        std::memcpy(&v, &f, 4);
        return v;
    }
    }
}

int cmd_afuzz(const std::vector<std::string>& args) {
    uint32_t fn = std::stoul(args.at(1), nullptr, 16);
    int count = args.size() > 2 ? std::stoi(args[2]) : 300;
    uint32_t seed = args.size() > 3 ? uint32_t(std::stoul(args[3])) : 1;
    std::vector<edgeunit::Hit> base;
    int seen = 0;
    edgeunit::for_each_call(harness::cap_list(args, 4), fn, [&](edgeunit::Hit& h) {
        if (seen++ % 4 == 0) base.push_back(h);
        return base.size() < 150;
    }, harness::edge_loaded);
    if (base.empty()) return 2;
    std::mt19937 g(seed);
    NativeFn nat = natives()[fn];
    int bad = 0, ran = 0, skipped = 0, declined = 0;
    std::FILE* dump = std::getenv("AFUZZ_DUMP") ? std::fopen(std::getenv("AFUZZ_DUMP"), "w") : nullptr;
    for (int it = 0; it < count; it++) {
        const edgeunit::Hit& h = base[g() % base.size()];
        edgeunit::Snapshot s = h.before;
        s.ls.resize(spu::LS_SIZE + 16);
        uint8_t* ls = s.ls.data();
        auto& gpr = s.regs.gpr;
        uint32_t ctx = gpr[3].u32[3] & 0x3fff0;
        uint32_t nv = (uint32_t(ls[ctx + 8]) << 8) | ls[ctx + 9];
        int rate = 1 + int(g() % 40);
        auto pokew = [&](uint32_t a, uint32_t n) {
            for (uint32_t i = 0; i < n; i++)
                if (g() % rate == 0) {
                    uint32_t x = (a + 4 * i) & 0x3fffc;
                    w32b(ls + x, specialf(g, r32b(ls + x)));
                }
        };
        auto pokeb = [&](uint32_t a, uint32_t n) {
            for (uint32_t i = 0; i < n; i++)
                if (g() % rate == 0) ls[(a + i) & 0x3ffff] = uint8_t(g());
        };
        int mode = int(g() % 4);
        if (fn == 0xe550) {
            uint32_t mats = gpr[4].u32[3], nm = gpr[5].u32[3], wts = gpr[6].u32[3];
            if (mode == 0 || mode == 3) pokew(mats, 16 * nm);
            if (mode == 1 || mode == 3) pokeb(wts, 8 * (nv + 4));
            if (mode == 2 || mode == 3) {
                pokew(r32b(ls + ctx + 92), 4 * (nv + 4));
                for (int t = 0; t < 3; t++)
                    if (uint32_t tb = r32b(ls + ctx + 96 + 4 * t)) pokew(tb, 4 * (nv + 4));
            }
        } else if (fn == 0xdbc8) {
            uint32_t verts = gpr[4].u32[3];
            if (mode != 3) pokeb(verts, 18 * (nv + 2));
            else pokeb(gpr[5].u32[3] & 0x3fff0, 32);
        } else if (fn == 0xa880) {
            for (int t = 1; t < 3; t++)
                if (uint32_t tb = r32b(ls + ctx + 96 + 4 * t)) pokew(tb, 4 * (nv + 8));
        } else if (fn == 0xd158) {
            for (int sl = 0; sl < 16 && ls[ctx + 352 + sl]; sl++) pokew(r32b(ls + ctx + 16 + 4 * sl), 4 * (nv + 2));
        } else if (fn == 0xf538) {
            uint32_t n = gpr[3].u32[3];
            if (mode != 1) pokew(gpr[4].u32[3], 4 * (n + 8));
            if (mode != 0) pokew(gpr[6].u32[3], 16);
        } else {
            pokew(gpr[4].u32[3], 4 * (nv + 8));
            pokew(gpr[5].u32[3], 4 * (nv + 8));
        }
        edgeunit::Snapshot iso = s;
        edgeunit::IsoResult r = edgeunit::run_isolated(iso, h.ret, h.sp);
        if (!r.ok) {
            skipped++;
            continue;
        }
        edgeunit::Snapshot ns = s;
        ns.regs.ls = ns.ls.data();
        edge::Outputs outs;
        bool took = nat(ns.ls.data(), ns.regs, outs);
        if (dump) {
            uint64_t hv = 0xcbf29ce484222325ull;
            for (size_t i = 0; i < spu::LS_SIZE; i++) hv = (hv ^ ns.ls[i]) * 0x100000001b3ull;
            const uint8_t* rp = reinterpret_cast<const uint8_t*>(&ns.regs.gpr[3]);
            for (int i = 0; i < 16; i++) hv = (hv ^ rp[i]) * 0x100000001b3ull;
            std::fprintf(dump, "%d %d %016llx\n", it, took ? 1 : 0, (unsigned long long)hv);
        }
        if (!took) {
            declined++;
            continue;
        }
        ran++;
        edgeunit::LsDiff d = edgeunit::diff_ls(ns.ls.data(), iso.ls.data(), h.sp);
        bool r3 = std::memcmp(&ns.regs.gpr[3], &iso.regs.gpr[3], 16) == 0;
        if (d.bytes || !r3) {
            bad++;
            if (bad <= 8) std::printf("  case %d mode %d job %d rate %d: ls %zu [%05x..%05x] r3 %s\n", it, mode, h.job, rate, d.bytes, d.first, d.last, r3 ? "ok" : "DIFF");
            if (bad <= 3 && std::getenv("AFUZZ_VERBOSE")) {
                int shown = 0;
                for (uint32_t a = d.first & ~3u; a <= d.last && shown < 6; a += 4)
                    if (std::memcmp(ns.ls.data() + a, iso.ls.data() + a, 4)) {
                        uint32_t q = a & ~15u;
                        std::printf("    %05x native %08x spu %08x  in %08x %08x %08x %08x\n", a, r32b(ns.ls.data() + a), r32b(iso.ls.data() + a), r32b(s.ls.data() + q), r32b(s.ls.data() + q + 4), r32b(s.ls.data() + q + 8), r32b(s.ls.data() + q + 12));
                        shown++;
                    }
            }
        }
    }
    if (dump) std::fclose(dump);
    std::printf("afuzz %05x: %d ran, %d declined, %d skipped, %d mismatching (avx2 %d)\n", fn, ran, declined, skipped, bad, spuf::avx2_on() ? 1 : 0);
    return bad ? 1 : 0;
}

CommandReg reg_afuzz("afuzz", cmd_afuzz);

}
