#pragma once

#include "cs_kern.h"

namespace cloth::kern {

using namespace cs::kern;

inline void fill16(V* R, uint8_t* ls, Ctl& ctl) {
    uint32_t bytes = pref(R[2]);
    if (bytes & 15) return;
    uint32_t m = bytes >> 4;
    if (m < 2) return;
    uint32_t k = m - 1;
    fill(ls, pref(R[5]), k, R[8], ctl);
    if (ctl.fail) return;
    R[2] = ai(R[2], -int32_t(16 * k));
    R[5] = ai(R[5], int32_t(16 * k));
}

inline void dist4(V* R, uint8_t* ls, Ctl& ctl) {
    uint32_t m = pref(R[47]) - pref(R[44]);
    if (m < 2 || m > (1u << 20)) return;
    if (!is_zero(R[35]) || !is_splat(R[57], 0x40000000u) || !is_splat(R[45], 0x3f800000u)) return;
    if (!is_const(R[40], 0x10100001u, 0x10101010u, 0x10101010u, 0x10101010u) || !is_const(R[39], 0x10100203u, 0x10101010u, 0x10101010u, 0x10101010u) || !is_const(R[38], 0x00000fffu, 0, 0, 0)) return;
    if (!is_const(R[56], 0x00010203u, 0x18191a1bu, 0x04050607u, 0x10111213u) || !is_const(R[54], 0x18191a1bu, 0x10111213u, 0x00010203u, 0x04050607u) || !is_const(R[55], 0x04050607u, 0x08090a0bu, 0x18191a1bu, 0x14151617u)) return;
    if (!is_const(R[49], 0x0c0d0e0fu, 0x00010203u, 0x10111213u, 0x1c1d1e1fu) || !is_const(R[50], 0x04050607u, 0x08090a0bu, 0x14151617u, 0x18191a1bu) || !is_const(R[51], 0x04050607u, 0x18191a1bu, 0x0c0d0e0fu, 0x80808080u)) return;
    if (!is_const(R[48], 0x1c1d1e1fu, 0x00010203u, 0x0c0d0e0fu, 0x80808080u) || !is_const(R[46], ~0u, ~0u, 0, 0) || !is_const(R[42], 0, ~0u, ~0u, 0) || !is_const(R[53], ~0u, 0, ~0u, 0) || !is_const(R[36], 0, 0, 0, ~0u)) return;
    __m128 kst = lanes(R[37]);
    __m128 eps = lanes(R[52]);
    __m128 acc = lanes(R[41]);
    if (!below(kst, kExp40) || !below(eps, kExp40) || !below(acc, kExp100)) return;
    const __m128 one = _mm_set1_ps(1.0f);
    const __m128 two = _mm_set1_ps(2.0f);
    const __m128i zero = _mm_setzero_si128();
    uint32_t base = pref(R[34]);
    uint32_t rec = pref(R[43]);
    uint32_t done = 0;
    for (; done + 1 < m; done++, rec += 64) {
        const uint8_t* q = ls + (rec & 0x3fff0);
        uint32_t w0 = spuf::ld32(q), w1 = spuf::ld32(q + 4), w2 = spuf::ld32(q + 8), w3 = spuf::ld32(q + 12);
        __m128 rest = spuf::ld4(ls + ((rec + 16) & 0x3fff0));
        __m128 wa = spuf::ld4(ls + ((rec + 32) & 0x3fff0));
        __m128 wb = spuf::ld4(ls + ((rec + 48) & 0x3fff0));
        uint32_t pa0 = (base + ((w0 >> 16) << 6)) & 0x3fff0, pb0 = (base + ((w0 & 0xfff) << 6)) & 0x3fff0;
        uint32_t pa1 = (base + ((w1 >> 16) << 6)) & 0x3fff0, pb1 = (base + ((w1 & 0xfff) << 6)) & 0x3fff0;
        uint32_t pa2 = (base + ((w2 >> 16) << 6)) & 0x3fff0, pb2 = (base + ((w2 & 0xfff) << 6)) & 0x3fff0;
        uint32_t pa3 = (base + ((w3 >> 16) << 6)) & 0x3fff0, pb3 = (base + ((w3 & 0xfff) << 6)) & 0x3fff0;
        __m128 a0 = spuf::ld4(ls + pa0), b0 = spuf::ld4(ls + pb0), a1 = spuf::ld4(ls + pa1), b1 = spuf::ld4(ls + pb1);
        __m128 a2 = spuf::ld4(ls + pa2), b2 = spuf::ld4(ls + pb2), a3 = spuf::ld4(ls + pa3), b3 = spuf::ld4(ls + pb3);
        __m128i big = _mm_or_si128(_mm_or_si128(_mm_or_si128(over(a0, kExp40), over(b0, kExp40)), _mm_or_si128(over(a1, kExp40), over(b1, kExp40))), _mm_or_si128(_mm_or_si128(over(a2, kExp40), over(b2, kExp40)), _mm_or_si128(over(a3, kExp40), over(b3, kExp40))));
        big = _mm_or_si128(big, _mm_or_si128(_mm_or_si128(over(wa, kExp40), over(wb, kExp40)), _mm_or_si128(over(rest, kExp40), under(rest, kExpM40))));
        if (!_mm_testz_si128(big, big)) break;
        __m128 x = _mm_sub_ps(b0, a0), y = _mm_sub_ps(b1, a1), z = _mm_sub_ps(b2, a2), t = _mm_sub_ps(b3, a3);
        _MM_TRANSPOSE4_PS(x, y, z, t);
        __m128 r72 = _mm_mul_ps(two, rest);
        __m128 r11 = _mm_div_ps(one, r72);
        __m128 r61 = _mm_fmadd_ps(_mm_fnmadd_ps(r11, r72, one), r11, r11);
        __m128 len2 = _mm_fmadd_ps(x, x, _mm_fmadd_ps(y, y, _mm_fmadd_ps(z, z, eps)));
        __m128 r9 = _mm_fnmadd_ps(_mm_fmsub_ps(rest, rest, len2), r61, rest);
        __m128 r32 = _mm_div_ps(one, r9);
        __m128 r3 = _mm_fmadd_ps(_mm_fnmadd_ps(r32, r9, one), r32, r32);
        __m128 s = _mm_fnmadd_ps(r3, rest, one);
        __m128 cx = _mm_mul_ps(_mm_mul_ps(x, s), kst);
        __m128 cy = _mm_mul_ps(_mm_mul_ps(y, s), kst);
        __m128 cz = _mm_mul_ps(_mm_mul_ps(z, s), kst);
        __m128 ax = _mm_mul_ps(cx, wa), ay = _mm_mul_ps(cy, wa), az = _mm_mul_ps(cz, wa), aw = _mm_setzero_ps();
        __m128 bx = _mm_mul_ps(cx, wb), by = _mm_mul_ps(cy, wb), bz = _mm_mul_ps(cz, wb), bw = _mm_setzero_ps();
        _MM_TRANSPOSE4_PS(ax, ay, az, aw);
        _MM_TRANSPOSE4_PS(bx, by, bz, bw);
        stf4(ls, pa0, spuf::pos_zero(_mm_add_ps(a0, ax)), ctl);
        stf4(ls, pb0, spuf::pos_zero(_mm_sub_ps(b0, bx)), ctl);
        stf4(ls, pa1, spuf::pos_zero(_mm_add_ps(a1, ay)), ctl);
        stf4(ls, pb1, spuf::pos_zero(_mm_sub_ps(b1, by)), ctl);
        stf4(ls, pa2, spuf::pos_zero(_mm_add_ps(a2, az)), ctl);
        stf4(ls, pb2, spuf::pos_zero(_mm_sub_ps(b2, bz)), ctl);
        stf4(ls, pa3, spuf::pos_zero(_mm_add_ps(a3, aw)), ctl);
        stf4(ls, pb3, spuf::pos_zero(_mm_sub_ps(b3, bw)), ctl);
        __m128i si = _mm_castps_si128(s);
        __m128 pos = _mm_castsi128_ps(_mm_and_si128(si, _mm_cmpgt_epi32(si, zero)));
        __m128 neg = _mm_castsi128_ps(_mm_and_si128(si, _mm_cmpgt_epi32(zero, si)));
        acc = spuf::pos_zero(_mm_sub_ps(spuf::pos_zero(_mm_add_ps(acc, pos)), neg));
        if (ctl.fail) return;
    }
    if (!done) return;
    R[43] = ai(R[43], int32_t(64 * done));
    R[44] = ai(R[44], int32_t(done));
    R[41] = unlanes(acc);
}

}
