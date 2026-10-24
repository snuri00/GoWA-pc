#pragma once

#include "renjob_chan.h"
#include "spu/context.h"

#include <cstdint>

namespace renjob {

struct WholeResult {
    bool done = false;
    uint32_t fail_pc = 0;
    uint32_t icall_miss = 0;
};

constexpr uint32_t kWholeEntry = 0x14cf8;
constexpr uint32_t kWholeTail = 0x14da4;
constexpr uint32_t kJobReturn = 0x3368;

WholeResult run_whole(uint8_t* ls, spu::Context& c, ChanHost& h);

}
