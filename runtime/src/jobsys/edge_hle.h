#pragma once

#include "edge_exec.h"

#include <cstdint>

namespace spu {
class Host;
}

namespace jsedge::hle {

bool enabled();
void counters(uint64_t fn_fallbacks[3], uint64_t& restarts, uint64_t& late_restarts);
bool run(Executor& x, spu::Host& h, const JobRef& j, const Cmd& c);
void prof_note(const char* name, uint64_t ticks);

}
