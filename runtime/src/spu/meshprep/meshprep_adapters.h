#pragma once

#include "spu/context.h"

#include <cstddef>
#include <cstdint>

namespace meshprep {

enum Job { kMeshPrep, kPrimBuilder, kGroupVis, kSortPrim, kJobCount };

struct JobInfo {
    const char* name;
    uint32_t ea;
    uint32_t size;
    uint32_t entry;
    uint32_t code_end;
    uint32_t header[12];
};

using AdapterFn = bool (*)(uint8_t* ls, spu::Context& c);

struct Adapter {
    uint32_t lsa;
    uint32_t sig_words;
    const uint32_t* signature;
    AdapterFn handler;
    const char* name;
    int job;
};

const JobInfo& job_info(int job);
int job_by_name(const char* name);
bool image_loaded(int job, const uint8_t* ls);
const Adapter* adapters(size_t& count);
uint32_t last_decline_pc();
uint32_t budget_exits();
void set_loop_budget(uint64_t n);

}
