#pragma once

#include <cstdint>

namespace edge {

void transform_vertexes(uint8_t* ls, uint32_t count, uint32_t src, uint32_t dst, uint32_t matrix, uint32_t r3_out[4]);

void compute_binormals(uint8_t* ls, uint32_t ctx, int32_t count, uint32_t r3_out[4]);

struct SegmentInitArgs {
    uint32_t r[20];
};

void init_segment(uint8_t* ls, const SegmentInitArgs& a);

bool normalize_vertexes(uint8_t* ls, uint32_t table, const uint32_t count[4], uint32_t r3_out[4]);

}
