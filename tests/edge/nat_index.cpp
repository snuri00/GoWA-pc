#include "natreg.h"
#include "unit.h"

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {


int cmd_bench_dcb0(const std::vector<std::string>& args) {
    int maxhits = args.size() > 1 ? std::stoi(args[1]) : 200;
    int rounds = args.size() > 2 ? std::stoi(args[2]) : 20;
    struct Case {
        std::vector<uint8_t> ls;
        uint32_t ctx, src;
    };
    std::vector<Case> cases;
    uint64_t spu = 0;
    edgeunit::for_each_call(harness::cap_list(args, 3), 0xdcb0, [&](edgeunit::Hit& h) {
        Case c;
        c.ls = h.before.ls;
        c.ls.resize(0x40010);
        c.ctx = h.before.regs.gpr[3].u32[3] & 0x3fff0;
        c.src = h.before.regs.gpr[4].u32[3] & 0x3ffff;
        spu += h.fn_insns;
        cases.push_back(std::move(c));
        return int(cases.size()) < maxhits;
    }, harness::edge_loaded);
    std::vector<uint8_t> work(0x40010);
    double best = 1e30;
    for (int r = 0; r < rounds; r++) {
        double t = 0;
        for (Case& c : cases) {
            uint32_t lo = c.src, hi = std::min<uint32_t>(c.src + 0x6000, 0x40000);
            std::memcpy(work.data() + lo, c.ls.data() + lo, hi - lo);
            std::memcpy(work.data() + c.ctx, c.ls.data() + c.ctx, 0x200);
            auto t0 = std::chrono::steady_clock::now();
            edge::decompress_indexes(work.data(), c.ctx, c.src, nullptr);
            t += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        }
        best = std::min(best, t);
    }
    std::printf("dcb0: %zu calls, %.2f us/call native (best of %d), %.0f SPU insns/call\n", cases.size(), best * 1e6 / cases.size(), rounds, double(spu) / cases.size());
    return 0;
}

CommandReg cmd_dcb0("bench_dcb0", cmd_bench_dcb0);

int cmd_fuzz_dcb0(const std::vector<std::string>& args) {
    int count = args.size() > 1 ? std::stoi(args[1]) : 1000;
    uint32_t seed = args.size() > 2 ? uint32_t(std::stoul(args[2])) : 1;
    edgeunit::Hit base;
    bool have = false;
    edgeunit::for_each_call(harness::cap_list(args, 3), 0xdcb0, [&](edgeunit::Hit& h) {
        base = h;
        have = true;
        return false;
    }, harness::edge_loaded);
    if (!have) return 2;
    uint64_t state = seed * 0x9E3779B97F4A7C15ull + 1;
    auto rnd = [&]() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return uint32_t(state >> 16);
    };
    int bad = 0;
    uint32_t ctx = base.before.regs.gpr[3].u32[3] & 0x3fff0;
    for (int it = 0; it < count; it++) {
        edgeunit::Snapshot s = base.before;
        s.ls.resize(0x40010);
        uint32_t src = 0x11000 + 16 * (rnd() % 0x1000);
        uint32_t ntri = 1 + rnd() % (rnd() % 4 == 0 ? 20 : 2000);
        uint32_t ni = ntri * 3;
        s.ls[ctx + 400] = uint8_t(ni >> 8);
        s.ls[ctx + 401] = uint8_t(ni);
        uint32_t n = rnd() % 4 == 0 ? rnd() % 8 : rnd() % 2800;
        uint32_t w = rnd() % 16;
        if (n == 0) w = 0;
        if (std::getenv("FUZZ_W")) w = uint32_t(std::atoi(std::getenv("FUZZ_W")));
        uint32_t fb = rnd() % 4 == 0 ? rnd() % 6 : rnd() % 600;
        uint32_t bias = rnd() & 0xffff;
        uint8_t* p = s.ls.data() + src;
        uint32_t total = 8 + fb + ((2 * ntri + 7) >> 3) + (n * w + 7) / 8 + 64;
        for (uint32_t i = 0; i < total && src + i < 0x40000; i++) p[i] = uint8_t(rnd());
        p[0] = uint8_t(n >> 8);
        p[1] = uint8_t(n);
        p[2] = uint8_t(bias >> 8);
        p[3] = uint8_t(bias);
        p[4] = uint8_t(fb >> 8);
        p[5] = uint8_t(fb);
        p[6] = uint8_t(w);
        s.regs.gpr[4].u32[3] = src;
        edgeunit::Snapshot iso = s;
        edgeunit::IsoResult r = edgeunit::run_isolated(iso, base.ret, base.sp);
        if (!r.ok) {
            std::printf("case %d: spu run failed %s\n", it, r.error.c_str());
            continue;
        }
        std::vector<uint8_t> nat = s.ls;
        edge::decompress_indexes(nat.data(), ctx, src, nullptr);
        edgeunit::LsDiff d = edgeunit::diff_ls(nat.data(), iso.ls.data(), base.sp);
        if (d.bytes) {
            bad++;
            if (bad <= 10) std::printf("case %d: src %05x ntri %u n %u w %u fb %u: %zu bytes differ [%05x..%05x]\n", it, src, ntri, n, w, fb, d.bytes, d.first, d.last);
            if (bad == 1) {
                uint32_t a0 = d.first & ~15u;
                for (uint32_t a = a0; a < a0 + 64; a += 16) {
                    std::printf("  %05x spu", a);
                    for (int k = 0; k < 16; k++) std::printf(" %02x", iso.ls[a + k]);
                    std::printf("\n        nat");
                    for (int k = 0; k < 16; k++) std::printf(" %02x", nat[a + k]);
                    std::printf("\n");
                }
                uint32_t pk = src + 8 + fb + ((2 * ntri + 7) >> 3);
                std::printf("  ubase %05x packed %05x bytes:", src + ((8 + fb + ((2 * ntri + 7) >> 3) + 15) & ~15u), pk);
                for (int k = 0; k < 24; k++) std::printf(" %02x", s.ls[pk + k]);
                std::printf(" bias %04x\n", bias);
            }
        }
    }
    std::printf("fuzz_dcb0: %d cases, %d mismatching\n", count, bad);
    return bad ? 1 : 0;
}

CommandReg cmd_fuzz("fuzz_dcb0", cmd_fuzz_dcb0);

int cmd_stats_dcb0(const std::vector<std::string>& args) {
    std::map<uint32_t, uint32_t> width, flavor;
    uint32_t zero_n = 0, total = 0, max_ntri = 0, max_n = 0;
    edgeunit::for_each_call(harness::cap_list(args, 1), 0xdcb0, [&](edgeunit::Hit& h) {
        const uint8_t* ls = h.before.ls.data();
        uint32_t ctx = h.before.regs.gpr[3].u32[3] & 0x3fff0, src = h.before.regs.gpr[4].u32[3] & 0x3ffff;
        uint32_t n = (ls[src] << 8) | ls[src + 1];
        uint32_t ntri = (((ls[ctx + 400] << 8) | ls[ctx + 401]) * 0xaaabu) & 0xffff;
        width[ls[src + 6]]++;
        flavor[ls[ctx + 6] >> 4]++;
        zero_n += n == 0;
        max_ntri = std::max(max_ntri, ntri);
        max_n = std::max(max_n, n);
        total++;
        return true;
    }, harness::edge_loaded);
    std::printf("calls %u, n==0 %u, max ntri %u, max n %u\n", total, zero_n, max_ntri, max_n);
    for (auto& [w, c] : width) std::printf("  width %2u: %u\n", w, c);
    for (auto& [f, c] : flavor) std::printf("  indexes flavor %u: %u\n", f, c);
    return 0;
}

CommandReg cmd_stats("stats_dcb0", cmd_stats_dcb0);

}
