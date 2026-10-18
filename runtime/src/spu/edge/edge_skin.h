#pragma once

#include <cstdint>

namespace edge {

bool skin_vertexes(uint8_t* ls, uint32_t ctx, uint32_t matrices, uint32_t num_matrices, uint32_t weights);
uint32_t skin_test_rsq(uint32_t seed, uint32_t n);

}
