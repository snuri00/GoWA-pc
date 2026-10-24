#pragma once

#include "renjob_chan.h"
#include "spu/context.h"

#include <cstdint>

namespace wj {

enum JobId : int { kPrimBuilder, kGroupVis, kSortPrim, kMeshPrep, kMeshHier, kShadows, kAnim, kScreamMp3, kScreamAux, kJobCount };

struct Info {
    const char* name;
    uint32_t img_ea;
    uint32_t img_size;
    uint32_t entry;
    uint32_t tail;
    const uint32_t* header;
    const uint32_t* entry_sig;
};

struct Result {
    bool done = false;
    uint32_t fail_pc = 0;
    uint32_t icall_miss = 0;
};

constexpr uint32_t kJobReturn = 0x3368;

const Info& info(int job);
int job_by_ea(uint32_t img_ea);
int job_in_ls(const uint8_t* ls);
Result run(int job, uint8_t* ls, spu::Context& c, renjob::ChanHost& h, bool load_next_job, spu::Host* ext_host = nullptr);
uint64_t ext_stats(uint64_t& steps);

}
