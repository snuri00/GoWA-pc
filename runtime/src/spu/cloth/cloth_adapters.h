#pragma once

#include "spu/context.h"

#include <cstddef>
#include <cstdint>

namespace cloth {

using AdapterFn = bool (*)(uint8_t* ls, spu::Context& c);

struct Adapter {
    uint32_t lsa;
    uint32_t sig_words;
    const uint32_t* signature;
    AdapterFn handler;
    const char* name;
};

const Adapter* adapters(size_t& count);
bool image_loaded(const uint8_t* ls);
uint32_t last_decline_pc();

}
