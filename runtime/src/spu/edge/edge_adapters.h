#pragma once

#include "edge_native.h"
#include "spu/context.h"

#include <cstddef>
#include <cstdint>

namespace edge {

using AdapterFn = bool (*)(uint8_t* ls, spu::Context& c, Outputs& outs);

struct Adapter {
    uint32_t lsa;
    uint32_t sig_words;
    uint64_t sig_hash;
    AdapterFn handler;
    const char* name;
};

const Adapter* adapters(size_t& count);
bool image_loaded(const uint8_t* ls);

}
