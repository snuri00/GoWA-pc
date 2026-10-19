#pragma once

#include <cstdint>

namespace edge {

struct Range {
    uint32_t lo = 0;
    uint32_t hi = 0;
};

struct Outputs {
    Range r[16];
    int n = 0;
    void add(uint32_t lo, uint32_t hi) {
        if (n < 16 && hi > lo) r[n++] = {lo, hi};
    }
};

bool decompress_indexes_fast(uint8_t* ls, uint32_t ctx, uint32_t src, uint32_t* end_out);
bool decompress_indexes_supported(const uint8_t* ls, uint32_t src);
uint32_t decompress_indexes(uint8_t* ls, uint32_t ctx, uint32_t src, Outputs* outs = nullptr);

}

#include "edge_cull.h"
#include "edge_skin.h"
#include "edge_vcomp.h"
#include "edge_vdecomp.h"
#include "edge_xform.h"
