#pragma once

#include "spu/context.h"

#include <cstddef>
#include <cstdint>

namespace scream {

using AdapterFn = bool (*)(uint8_t* ls, spu::Context& c);

struct Adapter {
    uint32_t lsa;
    uint32_t sig_words;
    const uint32_t* signature;
    AdapterFn handler;
    const char* name;
    int image;
};

const Adapter* adapters(size_t& count);
bool image_loaded(const uint8_t* ls, int image);
uint32_t last_decline_pc();

}
