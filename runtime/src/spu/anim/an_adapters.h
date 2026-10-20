#pragma once

#include "spu/context.h"

#include <cstddef>
#include <cstdint>

namespace anim {

constexpr uint64_t kDefaultBudget = 1ull << 16;

bool image_loaded(const uint8_t* ls);
bool run_hook(spu::Context& c, spu::Host& h);
const uint32_t* hooks(size_t& count);
const uint32_t* signature(uint32_t lsa, uint32_t& words);

}
