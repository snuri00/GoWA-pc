#pragma once

#include "core.h"

#include "spu/thread.h"

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace jobsys {

struct ChanRec {
    int kind;
    uint32_t ch;
    uint32_t value;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> lines;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> outs;
    std::vector<uint32_t> line_ea;
    std::vector<uint32_t> out_ea;
    bool ok = true;
};

struct Recorder : spu::Host {
    spu::SpuThread& t;
    std::vector<ChanRec> ev;
    uint32_t lsa = 0, eal = 0, size = 0;
    explicit Recorder(spu::SpuThread& th) : t(th) {}
    uint32_t read_channel(uint32_t ch) override;
    void write_channel(uint32_t ch, uint32_t value) override;
    uint32_t channel_count(uint32_t ch) override;
    bool stop_and_signal(uint32_t code) override;
    void halt(uint32_t pc, uint32_t word) override;
};

namespace verify {
void poison(spu::SpuThread& t, uint64_t seed, uint32_t mask);
bool run_verified(Executor& x, const JobDesc& j, Tier tier);
void report();
}

}
