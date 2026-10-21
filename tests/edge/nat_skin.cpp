#include "edge_skin.h"
#include "natreg.h"
#include "unit.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {


class CountHost : public spu::Host {
public:
    uint32_t read_channel(uint32_t) override { return 0; }
    void write_channel(uint32_t, uint32_t) override {}
    uint32_t channel_count(uint32_t) override { return 0; }
    bool stop_and_signal(uint32_t) override { return false; }
    void halt(uint32_t, uint32_t) override {}
};

int cmd_skincov(const std::vector<std::string>& args) {
    std::vector<uint32_t> pcs;
    for (const char* p : {"e684", "e788", "e858", "e970", "e9d8", "ea1c", "eb5c", "ede8", "edf0", "ee28", "efac", "f030", "f100", "f244", "f3fc", "f440", "f46c", "f478", "f484", "f49c", "f4ac", "f4bc"}) pcs.push_back(std::stoul(p, nullptr, 16));
    std::map<uint32_t, uint64_t> calls;
    std::map<std::string, int> combos;
    edgeunit::for_each_call(harness::cap_list(args, 1), 0xe550, [&](edgeunit::Hit& h) {
        edgeunit::Snapshot s = h.before;
        s.ls.resize(0x40010);
        s.regs.ls = s.ls.data();
        CountHost host;
        std::map<uint32_t, uint64_t> c;
        for (uint64_t k = 0; k < 50000000; k++) {
            uint32_t pc = s.regs.pc;
            c[pc]++;
            spu::step(s.regs, host);
            if (s.regs.pc == h.ret && s.regs.gpr[1].u32[3] == h.sp) break;
        }
        std::string key;
        for (uint32_t p : pcs)
            if (c.count(p)) {
                char b[16];
                std::snprintf(b, sizeof b, "%05x ", p);
                key += b;
                calls[p]++;
            }
        combos[key]++;
        return true;
    }, harness::edge_loaded);
    for (auto& [k, n] : combos) std::printf("%5d  %s\n", n, k.c_str());
    return 0;
}

CommandReg reg_skincov("skincov", cmd_skincov);

int cmd_bench_e550(const std::vector<std::string>& args) {
    int maxhits = args.size() > 1 ? std::stoi(args[1]) : 200;
    int rounds = args.size() > 2 ? std::stoi(args[2]) : 10;
    struct Case {
        std::vector<uint8_t> ls;
        uint32_t ctx, m, n, w;
    };
    std::vector<Case> cases;
    uint64_t spu = 0;
    edgeunit::for_each_call(harness::cap_list(args, 3), 0xe550, [&](edgeunit::Hit& h) {
        Case c;
        c.ls = h.before.ls;
        c.ls.resize(0x40010);
        c.ctx = h.before.regs.gpr[3].u32[3] & 0x3fff0;
        c.m = h.before.regs.gpr[4].u32[3] & 0x3ffff;
        c.n = h.before.regs.gpr[5].u32[3];
        c.w = h.before.regs.gpr[6].u32[3] & 0x3ffff;
        spu += h.fn_insns;
        cases.push_back(std::move(c));
        return int(cases.size()) < maxhits;
    }, harness::edge_loaded);
    std::vector<uint8_t> work(0x40010);
    double best = 1e30;
    for (int r = 0; r < rounds; r++) {
        double t = 0;
        for (Case& c : cases) {
            std::memcpy(work.data(), c.ls.data(), 0x40010);
            auto t0 = std::chrono::steady_clock::now();
            edge::skin_vertexes(work.data(), c.ctx, c.m, c.n, c.w);
            t += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        }
        best = std::min(best, t);
    }
    std::printf("e550: %zu calls, %.2f us/call native (best of %d), %.0f SPU insns/call\n", cases.size(), best * 1e6 / cases.size(), rounds, double(spu) / cases.size());
    return 0;
}

CommandReg reg_bench_e550("bench_e550", cmd_bench_e550);

int cmd_rsq(const std::vector<std::string>&) {
    std::printf("bad %u\n", edge::skin_test_rsq(1, 1000000));
    return 0;
}

CommandReg reg_rsq("skinrsq", cmd_rsq);

}
