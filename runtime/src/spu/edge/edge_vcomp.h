#pragma once

#include "edge_native.h"

#include <cstdint>

namespace edge {

bool compress_vertexes(uint8_t* ls, uint32_t ctx, int overlay, uint32_t r3out[4], Outputs* outs = nullptr, bool* called = nullptr);
void compress_vertexes_stack(uint8_t* ls, const uint32_t regs[128][4], int overlay, bool called, const uint32_t r3out[4]);

}
