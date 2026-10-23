#pragma once

#include "ring.h"

#include <cstdint>

namespace edge::gpu {
struct Record;
}

namespace gpu::edge_compute {

bool enabled();
bool verify();
bool expand(const edge::gpu::Record& r, RingAlloc& out);
void add_check(const float* gpu, const float* cpu, uint32_t count, uint32_t ea);
void frame_complete();

}
