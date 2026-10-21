#pragma once

#include "edge_native.h"

#include <cstdint>

namespace edge {

struct VdRegs {
    uint8_t r1[16];
    uint8_t saved[10][16];
};

uint64_t overlay_hash(const uint8_t* p, uint32_t n);
int vdecomp_identify(const uint8_t* ls, uint32_t entry);
bool vdecomp_run(uint8_t* ls, int overlay, uint32_t ctx, uint32_t verts, uint32_t num_vertexes, uint32_t desc, uint32_t format, const VdRegs& regs, Outputs* outs = nullptr);

}
