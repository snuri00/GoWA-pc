#pragma once

#include "spu/context.h"
#include "../tasks/tk_adapters.h"

#include <cstddef>
#include <cstdint>

namespace screamjob {

constexpr uint64_t kDefaultBudget = 1ull << 16;

struct Image {
    const char* name;
    const uint32_t* hooks;
    size_t hook_count;
    const uint32_t* (*signature)(uint32_t lsa, uint32_t& words);
};

const Image* images(size_t& count);
bool run_hook(spu::Context& c, spu::Host& h);
const tasks::Image* task_images(size_t& count);

}
