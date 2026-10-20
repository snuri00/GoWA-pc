#pragma once

#include "spu/context.h"

#include <cstddef>
#include <cstdint>

namespace tasks {

constexpr uint64_t kDefaultBudget = 1ull << 16;

struct Image {
    const char* name;
    const uint32_t* (*hooks)(size_t& count);
    const uint32_t* (*signature)(uint32_t lsa, uint32_t& words);
    bool (*image_loaded)(const uint8_t* ls);
    bool (*run_hook)(spu::Context& c, spu::Host& h);
    uint32_t code_lo, code_hi;
};

const Image* images(size_t& count);

}
