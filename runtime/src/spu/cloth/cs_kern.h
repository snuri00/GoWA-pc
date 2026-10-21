#pragma once

#include "cs_ops.h"

#include <immintrin.h>

#include <cstdint>
#include <cstring>

namespace cs::kern {

using cs::xo::Ctl;
using cs::xo::V;
using cs::xo::ai;
using cs::xo::pref;

constexpr uint32_t kExp(int e) { return uint32_t(127 + e) << 23; }
constexpr uint32_t kExp40 = kExp(40);
constexpr uint32_t kExp60 = kExp(60);
constexpr uint32_t kExp100 = kExp(100);
constexpr uint32_t kExpM40 = kExp(-40);
constexpr uint32_t kExpM60 = kExp(-60);

inline __m128 lanes(V v) { return _mm_castsi128_ps(_mm_shuffle_epi32(v, 0x1b)); }
inline V unlanes(__m128 f) { return _mm_shuffle_epi32(_mm_castps_si128(f), 0x1b); }
inline __m128i ilanes(V v) { return _mm_shuffle_epi32(v, 0x1b); }
inline V unilanes(__m128i v) { return _mm_shuffle_epi32(v, 0x1b); }

inline V spu_const(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) { return _mm_set_epi32(int(w0), int(w1), int(w2), int(w3)); }
inline bool same(V a, V b) {
    __m128i x = _mm_xor_si128(a, b);
    return _mm_testz_si128(x, x);
}
inline bool is_const(V v, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) { return same(v, spu_const(w0, w1, w2, w3)); }
inline bool is_splat(V v, uint32_t w) { return same(v, _mm_set1_epi32(int(w))); }
inline bool is_zero(V v) { return _mm_testz_si128(v, v); }

inline __m128i absbits(__m128 f) { return _mm_and_si128(_mm_castps_si128(f), _mm_set1_epi32(0x7fffffff)); }
inline __m128i over(__m128 f, uint32_t lim) { return _mm_cmpgt_epi32(absbits(f), _mm_set1_epi32(int(lim - 1))); }
inline __m128i under(__m128 f, uint32_t lim) { return _mm_cmpgt_epi32(_mm_set1_epi32(int(lim)), absbits(f)); }
inline bool below(__m128 f, uint32_t lim) {
    __m128i m = over(f, lim);
    return _mm_testz_si128(m, m);
}

inline void stf4(uint8_t* ls, uint32_t a, __m128 f, Ctl& ctl) {
    a &= 0x3fff0;
    if (ctl.journal && !cs::xo::journal(ls, a, ctl)) [[unlikely]]
        return;
    spuf::st4(ls + a, f);
}

inline void stv(uint8_t* ls, uint32_t a, V v, Ctl& ctl) { cs::xo::st(ls, a, v, ctl); }

inline void fill(uint8_t* ls, uint32_t a, uint32_t quads, V pat, Ctl& ctl) {
    a &= 0x3fff0;
    if (ctl.journal || a + uint64_t(quads) * 16 > 0x40000) {
        for (uint32_t i = 0; i < quads && !ctl.fail; i++) cs::xo::st(ls, a + 16 * i, pat, ctl);
        return;
    }
    __m128i b = cs::xo::bswap(pat);
    __m256i b2 = _mm256_broadcastsi128_si256(b);
    uint8_t* p = ls + a;
    uint32_t i = 0;
    for (; i + 2 <= quads; i += 2) _mm256_storeu_si256(reinterpret_cast<__m256i*>(p + 16 * i), b2);
    if (i < quads) _mm_storeu_si128(reinterpret_cast<__m128i*>(p + 16 * i), b);
}

}
