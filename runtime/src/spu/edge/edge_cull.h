#pragma once

#include <cstdint>

namespace edge {

struct CullIn {
    uint32_t ctx = 0;
    uint8_t r0[16] = {};
    uint8_t r1[16] = {};
    uint8_t r4[16] = {};
    uint8_t r5[16] = {};
    uint8_t r80[16] = {};
    uint8_t r81[16] = {};
    uint8_t r82[16] = {};
    uint8_t r126[16] = {};
};

int32_t cull_triangles(uint8_t* ls, const CullIn& in);
int32_t cull_triangles_ref(uint8_t* ls, const CullIn& in);

}
