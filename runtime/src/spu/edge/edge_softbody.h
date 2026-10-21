#pragma once

#include <cstdint>

namespace edge {

bool soft_body(uint8_t* ls, void* gpr);
void soft_body_stats(uint64_t* fast, uint64_t* checked, uint64_t* exact, uint64_t* loops, uint64_t* loops_declined);

}
