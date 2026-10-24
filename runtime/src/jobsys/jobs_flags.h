#pragma once

#include "renjob_ops.h"

#include <cstdint>

namespace renjob::xo {

inline V sumb(V a, V b) {
    alignas(16) uint8_t x[16], y[16];
    alignas(16) uint16_t r[8];
    _mm_store_si128(reinterpret_cast<__m128i*>(x), a);
    _mm_store_si128(reinterpret_cast<__m128i*>(y), b);
    for (int i = 0; i < 4; i++) {
        uint16_t sb = 0, sa = 0;
        for (int k = 0; k < 4; k++) {
            sb = uint16_t(sb + y[i * 4 + k]);
            sa = uint16_t(sa + x[i * 4 + k]);
        }
        r[i * 2 + 1] = sb;
        r[i * 2] = sa;
    }
    return _mm_load_si128(reinterpret_cast<const __m128i*>(r));
}

}

namespace wj {

inline thread_local bool t_lnj = false;
uint32_t ext_call(void* R, uint8_t* ls, uint32_t target, uint32_t ret);

}
