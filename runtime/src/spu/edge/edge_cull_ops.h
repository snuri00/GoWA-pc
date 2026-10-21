#pragma once

#include "spu_float.h"

#include <immintrin.h>

#include <bit>
#include <cstdint>
#include <cstring>

namespace edge::xo {

using V = __m128i;

#define XO inline __attribute__((always_inline))

XO __m128 F(V x) { return _mm_castsi128_ps(x); }
XO V I(__m128 x) { return _mm_castps_si128(x); }
XO V bswap(V x) { return _mm_shuffle_epi8(x, _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15)); }
XO uint32_t pref(V v) { return uint32_t(_mm_extract_epi32(v, 3)); }
XO uint32_t hw6(V v) { return uint32_t(_mm_extract_epi16(v, 6)); }
XO V pref_only(uint32_t x) { return _mm_setr_epi32(0, 0, 0, int(x)); }
XO V lq(const uint8_t* ls, uint32_t a) { return bswap(_mm_loadu_si128(reinterpret_cast<const __m128i*>(ls + (a & 0x3fff0)))); }
XO void sq(uint8_t* ls, uint32_t a, V v) { _mm_storeu_si128(reinterpret_cast<__m128i*>(ls + (a & 0x3fff0)), bswap(v)); }

XO V s32(uint32_t v) { return _mm_set1_epi32(int(v)); }
XO V s16(uint32_t v) { return _mm_set1_epi16(short(v)); }
XO V s8(uint32_t v) { return _mm_set1_epi8(char(v)); }

XO V il(int32_t i) { return s32(uint32_t(i)); }
XO V ilh(uint32_t i) { return s16(i); }
XO V ilhu(uint32_t i) { return s32(i << 16); }
XO V ila(uint32_t i) { return s32(i); }
XO V iohl(V t, uint32_t i) { return _mm_or_si128(t, s32(i)); }
XO V fsmbi(uint32_t m) {
    if (spuf::avx2_on()) {
        V b = _mm_shuffle_epi8(_mm_cvtsi32_si128(int(m)), _mm_setr_epi8(0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1));
        V bit = _mm_setr_epi8(1, 2, 4, 8, 16, 32, 64, -128, 1, 2, 4, 8, 16, 32, 64, -128);
        return _mm_cmpeq_epi8(_mm_and_si128(b, bit), bit);
    }
    alignas(16) uint8_t b[16];
    for (int i = 0; i < 16; i++) b[15 - i] = (m >> (15 - i)) & 1 ? 0xFF : 0;
    return _mm_load_si128(reinterpret_cast<const __m128i*>(b));
}

XO V a(V x, V y) { return _mm_add_epi32(x, y); }
XO V ah(V x, V y) { return _mm_add_epi16(x, y); }
XO V ai(V x, int32_t i) { return _mm_add_epi32(x, s32(uint32_t(i))); }
XO V ahi(V x, int32_t i) { return _mm_add_epi16(x, s16(uint32_t(i))); }
XO V sf(V x, V y) { return _mm_sub_epi32(y, x); }
XO V sfh(V x, V y) { return _mm_sub_epi16(y, x); }
XO V sfi(V x, int32_t i) { return _mm_sub_epi32(s32(uint32_t(i)), x); }
XO V sfhi(V x, int32_t i) { return _mm_sub_epi16(s16(uint32_t(i)), x); }
XO V and_(V x, V y) { return _mm_and_si128(x, y); }
XO V andc(V x, V y) { return _mm_andnot_si128(y, x); }
XO V or_(V x, V y) { return _mm_or_si128(x, y); }
XO V xor_(V x, V y) { return _mm_xor_si128(x, y); }
XO V nor(V x, V y) { return _mm_xor_si128(_mm_or_si128(x, y), s32(0xffffffffu)); }
XO V nand(V x, V y) { return _mm_xor_si128(_mm_and_si128(x, y), s32(0xffffffffu)); }
XO V orc(V x, V y) { return _mm_or_si128(x, _mm_xor_si128(y, s32(0xffffffffu))); }
XO V eqv(V x, V y) { return _mm_xor_si128(_mm_xor_si128(x, y), s32(0xffffffffu)); }
XO V andi(V x, int32_t i) { return _mm_and_si128(x, s32(uint32_t(i))); }
XO V andhi(V x, int32_t i) { return _mm_and_si128(x, s16(uint32_t(i))); }
XO V andbi(V x, int32_t i) { return _mm_and_si128(x, s8(uint32_t(i))); }
XO V ori(V x, int32_t i) { return _mm_or_si128(x, s32(uint32_t(i))); }
XO V orhi(V x, int32_t i) { return _mm_or_si128(x, s16(uint32_t(i))); }
XO V orbi(V x, int32_t i) { return _mm_or_si128(x, s8(uint32_t(i))); }
XO V xori(V x, int32_t i) { return _mm_xor_si128(x, s32(uint32_t(i))); }
XO V xorhi(V x, int32_t i) { return _mm_xor_si128(x, s16(uint32_t(i))); }
XO V xorbi(V x, int32_t i) { return _mm_xor_si128(x, s8(uint32_t(i))); }

template <class F>
XO V map32(V x, V y, F f) {
    alignas(16) uint32_t a[4], b[4], r[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(a), x);
    _mm_store_si128(reinterpret_cast<__m128i*>(b), y);
    for (int i = 0; i < 4; i++) r[i] = f(a[i], b[i], i);
    return _mm_load_si128(reinterpret_cast<const __m128i*>(r));
}

XO V cg(V x, V y) { return _mm_srli_epi32(_mm_cmpgt_epi32(_mm_xor_si128(x, s32(0x80000000u)), _mm_xor_si128(_mm_add_epi32(x, y), s32(0x80000000u))), 31); }
XO V addx(V x, V y, V t) { return _mm_add_epi32(_mm_add_epi32(x, y), _mm_and_si128(t, s32(1))); }
XO V mpy(V x, V y) { return _mm_madd_epi16(_mm_and_si128(x, s32(0xffff)), _mm_and_si128(y, s32(0xffff))); }
XO V mpyu(V x, V y) { return _mm_mullo_epi32(_mm_and_si128(x, s32(0xffff)), _mm_and_si128(y, s32(0xffff))); }
XO V mpyi(V x, int32_t i) { return _mm_mullo_epi32(_mm_srai_epi32(_mm_slli_epi32(x, 16), 16), s32(uint32_t(int32_t(int16_t(i))))); }
XO V mpyui(V x, int32_t i) { return _mm_mullo_epi32(_mm_and_si128(x, s32(0xffff)), s32(uint32_t(i) & 0xffff)); }
XO V mpyh(V x, V y) { return _mm_slli_epi32(_mm_mullo_epi32(_mm_srli_epi32(x, 16), _mm_and_si128(y, s32(0xffff))), 16); }
XO V mpyhhu(V x, V y) { return _mm_mullo_epi32(_mm_srli_epi32(x, 16), _mm_srli_epi32(y, 16)); }
XO V mpyhh(V x, V y) { return _mm_mullo_epi32(_mm_srai_epi32(x, 16), _mm_srai_epi32(y, 16)); }

XO V cntb(V x) {
    __m128i lo = _mm_and_si128(x, s8(0x0f));
    __m128i hi = _mm_and_si128(_mm_srli_epi16(x, 4), s8(0x0f));
    __m128i t = _mm_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
    return _mm_add_epi8(_mm_shuffle_epi8(t, lo), _mm_shuffle_epi8(t, hi));
}
XO V xsbh(V x) { return _mm_srai_epi16(_mm_slli_epi16(x, 8), 8); }
XO V xshw(V x) { return _mm_srai_epi32(_mm_slli_epi32(x, 16), 16); }
XO V xswd(V x) {
    alignas(16) uint64_t q[2];
    _mm_store_si128(reinterpret_cast<__m128i*>(q), x);
    q[0] = uint64_t(int64_t(int32_t(q[0])));
    q[1] = uint64_t(int64_t(int32_t(q[1])));
    return _mm_load_si128(reinterpret_cast<const __m128i*>(q));
}

XO V shl(V x, V y) { return _mm_sllv_epi32(x, _mm_and_si128(y, s32(0x3f))); }
XO V shli(V x, int32_t i) {
    uint32_t s = uint32_t(i) & 0x3f;
    return s > 31 ? _mm_setzero_si128() : _mm_sll_epi32(x, _mm_cvtsi32_si128(int(s)));
}
XO V shlhi(V x, int32_t i) {
    uint32_t s = uint32_t(i) & 0x1f;
    return s > 15 ? _mm_setzero_si128() : _mm_sll_epi16(x, _mm_cvtsi32_si128(int(s)));
}
XO V roti(V x, int32_t i) {
    int s = i & 0x1f;
    return _mm_or_si128(_mm_sll_epi32(x, _mm_cvtsi32_si128(s)), _mm_srl_epi32(x, _mm_cvtsi32_si128(32 - s)));
}
XO V rot(V x, V y) {
    __m128i s = _mm_and_si128(y, s32(0x1f));
    return _mm_or_si128(_mm_sllv_epi32(x, s), _mm_srlv_epi32(x, _mm_sub_epi32(s32(32), s)));
}
XO V rotm(V x, V y) { return _mm_srlv_epi32(x, _mm_and_si128(_mm_sub_epi32(_mm_setzero_si128(), y), s32(0x3f))); }
XO V rotmi(V x, int32_t i) {
    uint32_t s = (0u - uint32_t(i)) & 0x3f;
    return s > 31 ? _mm_setzero_si128() : _mm_srl_epi32(x, _mm_cvtsi32_si128(int(s)));
}
XO V rotma(V x, V y) { return _mm_srav_epi32(x, _mm_min_epu32(_mm_and_si128(_mm_sub_epi32(_mm_setzero_si128(), y), s32(0x3f)), s32(31))); }
XO V rotmai(V x, int32_t i) {
    uint32_t s = (0u - uint32_t(i)) & 0x3f;
    return _mm_sra_epi32(x, _mm_cvtsi32_si128(int(s > 31 ? 31 : s)));
}
XO V rothmi(V x, int32_t i) {
    uint32_t s = (0u - uint32_t(i)) & 0x1f;
    return s > 15 ? _mm_setzero_si128() : _mm_srl_epi16(x, _mm_cvtsi32_si128(int(s)));
}
XO V rotmahi(V x, int32_t i) {
    uint32_t s = (0u - uint32_t(i)) & 0x1f;
    return _mm_sra_epi16(x, _mm_cvtsi32_si128(int(s > 15 ? 15 : s)));
}
inline V roth(V x, V y) {
    alignas(16) uint16_t a[8], b[8];
    _mm_store_si128(reinterpret_cast<__m128i*>(a), x);
    _mm_store_si128(reinterpret_cast<__m128i*>(b), y);
    for (int i = 0; i < 8; i++) a[i] = std::rotl(a[i], b[i] & 15);
    return _mm_load_si128(reinterpret_cast<const __m128i*>(a));
}
inline V rothm(V x, V y) {
    alignas(16) uint16_t a[8], b[8];
    _mm_store_si128(reinterpret_cast<__m128i*>(a), x);
    _mm_store_si128(reinterpret_cast<__m128i*>(b), y);
    for (int i = 0; i < 8; i++) {
        uint32_t s = (0u - b[i]) & 0x1f;
        a[i] = uint16_t(s > 15 ? 0 : a[i] >> s);
    }
    return _mm_load_si128(reinterpret_cast<const __m128i*>(a));
}
inline V rotmah(V x, V y) {
    alignas(16) uint16_t a[8], b[8];
    _mm_store_si128(reinterpret_cast<__m128i*>(a), x);
    _mm_store_si128(reinterpret_cast<__m128i*>(b), y);
    for (int i = 0; i < 8; i++) {
        uint32_t s = (0u - b[i]) & 0x1f;
        a[i] = uint16_t(int16_t(a[i]) >> (s > 15 ? 15 : s));
    }
    return _mm_load_si128(reinterpret_cast<const __m128i*>(a));
}

XO V iota8() { return _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15); }
XO V qshl_bytes(V x, uint32_t n) {
    if (n >= 16) return _mm_setzero_si128();
    return _mm_shuffle_epi8(x, _mm_sub_epi8(iota8(), s8(n)));
}
XO V qshr_bytes(V x, uint32_t n) {
    if (n >= 16) return _mm_setzero_si128();
    __m128i idx = _mm_add_epi8(iota8(), s8(n));
    idx = _mm_or_si128(idx, _mm_cmpgt_epi8(idx, s8(15)));
    return _mm_shuffle_epi8(x, idx);
}
XO V qrot_bytes(V x, uint32_t n) { return _mm_shuffle_epi8(x, _mm_and_si128(_mm_sub_epi8(iota8(), s8(n & 15)), s8(15))); }
inline V qshl_bits(V x, uint32_t n) {
    if (!n) return x;
    alignas(16) uint64_t q[2], r[2];
    _mm_store_si128(reinterpret_cast<__m128i*>(q), x);
    r[1] = (q[1] << n) | (q[0] >> (64 - n));
    r[0] = q[0] << n;
    return _mm_load_si128(reinterpret_cast<const __m128i*>(r));
}
XO V shlqbyi(V x, int32_t i) { return qshl_bytes(x, uint32_t(i) & 0x1f); }
XO V shlqby(V x, V y) { return qshl_bytes(x, pref(y) & 0x1f); }
XO V rotqbyi(V x, int32_t i) { return qrot_bytes(x, uint32_t(i) & 15); }
XO V rotqby(V x, V y) { return qrot_bytes(x, pref(y) & 15); }
XO V rotqmbyi(V x, int32_t i) { return qshr_bytes(x, (0u - uint32_t(i)) & 0x1f); }
XO V rotqmby(V x, V y) { return qshr_bytes(x, (0u - pref(y)) & 0x1f); }
XO V shlqbii(V x, int32_t i) { return qshl_bits(x, uint32_t(i) & 7); }

inline V insert_mask(uint32_t t, int size) {
    const __m128i base = _mm_setr_epi8(0x1F, 0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10);
    __m128i pat, msk;
    if (size == 1) {
        pat = _mm_setr_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3);
        msk = _mm_setr_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1);
    } else if (size == 2) {
        pat = _mm_setr_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 2);
        msk = _mm_setr_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1);
    } else if (size == 4) {
        pat = _mm_setr_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 2, 1, 0);
        msk = _mm_setr_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, -1);
    } else {
        pat = _mm_setr_epi8(0, 0, 0, 0, 0, 0, 0, 0, 7, 6, 5, 4, 3, 2, 1, 0);
        msk = _mm_setr_epi8(0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, -1, -1, -1, -1, -1);
    }
    uint32_t pos = (size == 1 ? t : (t & ~uint32_t(size - 1))) & 0xF;
    return _mm_blendv_epi8(base, qshr_bytes(pat, pos), qshr_bytes(msk, pos));
}
XO V cbd(V x, int32_t i) { return insert_mask(pref(x) + uint32_t(i), 1); }
XO V chd(V x, int32_t i) { return insert_mask(pref(x) + uint32_t(i), 2); }
XO V cwd(V x, int32_t i) { return insert_mask(pref(x) + uint32_t(i), 4); }
XO V cdd(V x, int32_t i) { return insert_mask(pref(x) + uint32_t(i), 8); }

XO V ceq(V x, V y) { return _mm_cmpeq_epi32(x, y); }
XO V ceqh(V x, V y) { return _mm_cmpeq_epi16(x, y); }
XO V ceqb(V x, V y) { return _mm_cmpeq_epi8(x, y); }
XO V ceqi(V x, int32_t i) { return _mm_cmpeq_epi32(x, s32(uint32_t(i))); }
XO V ceqhi(V x, int32_t i) { return _mm_cmpeq_epi16(x, s16(uint32_t(i))); }
XO V ceqbi(V x, int32_t i) { return _mm_cmpeq_epi8(x, s8(uint32_t(i))); }
XO V cgt(V x, V y) { return _mm_cmpgt_epi32(x, y); }
XO V cgth(V x, V y) { return _mm_cmpgt_epi16(x, y); }
XO V cgtb(V x, V y) { return _mm_cmpgt_epi8(x, y); }
XO V cgti(V x, int32_t i) { return _mm_cmpgt_epi32(x, s32(uint32_t(i))); }
XO V cgthi(V x, int32_t i) { return _mm_cmpgt_epi16(x, s16(uint32_t(i))); }
XO V cgtbi(V x, int32_t i) { return _mm_cmpgt_epi8(x, s8(uint32_t(i))); }
XO V clgt(V x, V y) { return _mm_cmpgt_epi32(_mm_xor_si128(x, s32(0x80000000u)), _mm_xor_si128(y, s32(0x80000000u))); }
XO V clgth(V x, V y) { return _mm_cmpgt_epi16(_mm_xor_si128(x, s16(0x8000)), _mm_xor_si128(y, s16(0x8000))); }
XO V clgtb(V x, V y) { return _mm_cmpgt_epi8(_mm_xor_si128(x, s8(0x80)), _mm_xor_si128(y, s8(0x80))); }
XO V clgti(V x, int32_t i) { return _mm_cmpgt_epi32(_mm_xor_si128(x, s32(0x80000000u)), _mm_xor_si128(s32(uint32_t(i)), s32(0x80000000u))); }
XO V clgthi(V x, int32_t i) { return _mm_cmpgt_epi16(_mm_xor_si128(x, s16(0x8000)), _mm_xor_si128(s16(uint32_t(i)), s16(0x8000))); }
XO V clgtbi(V x, int32_t i) { return _mm_cmpgt_epi8(_mm_xor_si128(x, s8(0x80)), _mm_xor_si128(s8(uint32_t(i)), s8(0x80))); }

XO V selb(V x, V y, V m) { return _mm_or_si128(_mm_andnot_si128(m, x), _mm_and_si128(m, y)); }
XO V shufb(V x, V y, V ctl) {
    __m128i idx = _mm_andnot_si128(ctl, s8(0x0f));
    __m128i ra = _mm_shuffle_epi8(x, idx);
    __m128i rb = _mm_shuffle_epi8(y, idx);
    __m128i r = _mm_blendv_epi8(ra, rb, _mm_slli_epi16(ctl, 3));
    if (_mm_movemask_epi8(ctl) == 0) [[likely]]
        return r;
    __m128i t = _mm_and_si128(ctl, s8(0xE0));
    __m128i special = _mm_or_si128(_mm_cmpeq_epi8(t, s8(0xC0)), _mm_and_si128(_mm_cmpeq_epi8(t, s8(0xE0)), s8(0x80)));
    return _mm_blendv_epi8(r, special, ctl);
}
XO V fsmb(V x) {
    uint32_t mv = pref(x) & 0xFFFF;
    __m128i b = _mm_shuffle_epi8(_mm_cvtsi32_si128(int(mv)), _mm_setr_epi8(0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1));
    __m128i bits = _mm_setr_epi8(1, 2, 4, 8, 16, 32, 64, -128, 1, 2, 4, 8, 16, 32, 64, -128);
    return _mm_cmpeq_epi8(_mm_and_si128(b, bits), bits);
}
inline V fsmh(V x) {
    uint32_t m = pref(x) & 0xFF;
    alignas(16) uint16_t r[8];
    for (int i = 0; i < 8; i++) r[7 - i] = (m >> (7 - i)) & 1 ? 0xFFFF : 0;
    return _mm_load_si128(reinterpret_cast<const __m128i*>(r));
}
inline V fsm(V x) {
    uint32_t m = pref(x) & 0xF;
    return _mm_setr_epi32((m & 1) ? -1 : 0, (m & 2) ? -1 : 0, (m & 4) ? -1 : 0, (m & 8) ? -1 : 0);
}
XO V gb(V x) { return pref_only(uint32_t(_mm_movemask_ps(F(_mm_slli_epi32(x, 31))))); }
XO V gbh(V x) { return pref_only(uint32_t(_mm_movemask_epi8(_mm_packs_epi16(_mm_slli_epi16(x, 15), _mm_setzero_si128())))); }

XO V fa(V x, V y) { return I(spuf::fa(F(x), F(y))); }
XO V fs(V x, V y) { return I(spuf::fs(F(x), F(y))); }
XO V fm(V x, V y) { return I(spuf::fm(F(x), F(y))); }
XO V fma(V x, V y, V z) { return I(spuf::fma(F(x), F(y), F(z))); }
XO V fms(V x, V y, V z) { return I(spuf::fms(F(x), F(y), F(z))); }
XO V fnms(V x, V y, V z) { return I(spuf::fnms(F(x), F(y), F(z))); }
XO V smax_fix(V r) {
    __m128i e = _mm_cmpeq_epi32(_mm_and_si128(r, s32(0x7F800000)), s32(0x7F800000));
    return _mm_blendv_epi8(r, _mm_or_si128(_mm_and_si128(r, s32(0x80000000u)), s32(0x7FFFFFFF)), e);
}
XO V frest(V x) { return smax_fix(I(_mm_div_ps(_mm_set1_ps(1.0f), F(x)))); }
XO V frsqest(V x) { return smax_fix(I(_mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(F(_mm_and_si128(x, s32(0x7FFFFFFF))))))); }
XO V fi(V, V y) { return y; }
XO V fkey(V v) {
    __m128i mag = _mm_and_si128(v, s32(0x7FFFFFFF));
    __m128i zero = _mm_cmpeq_epi32(_mm_and_si128(v, s32(0x7F800000)), _mm_setzero_si128());
    mag = _mm_andnot_si128(zero, mag);
    __m128i neg = _mm_srai_epi32(v, 31);
    return _mm_sub_epi32(_mm_xor_si128(mag, neg), neg);
}
XO V fmag(V v) {
    __m128i mag = _mm_and_si128(v, s32(0x7FFFFFFF));
    __m128i zero = _mm_cmpeq_epi32(_mm_and_si128(v, s32(0x7F800000)), _mm_setzero_si128());
    return _mm_andnot_si128(zero, mag);
}
XO V fcgt(V x, V y) { return _mm_cmpgt_epi32(fkey(x), fkey(y)); }
XO V fceq(V x, V y) { return _mm_cmpeq_epi32(fkey(x), fkey(y)); }
XO V fcmgt(V x, V y) { return _mm_cmpgt_epi32(fmag(x), fmag(y)); }
XO V fcmeq(V x, V y) { return _mm_cmpeq_epi32(fmag(x), fmag(y)); }
XO V cvt_mag(V x, int i8, V& E) {
    V e = _mm_and_si128(_mm_srli_epi32(x, 23), s32(0xff));
    V ez = _mm_cmpeq_epi32(e, _mm_setzero_si128());
    E = _mm_andnot_si128(ez, _mm_add_epi32(e, s32(uint32_t(46 - i8))));
    V mant = _mm_or_si128(_mm_and_si128(x, s32(0x7fffff)), s32(0x800000));
    V d = _mm_sub_epi32(s32(23), E);
    V r = _mm_or_si128(_mm_srlv_epi32(mant, d), _mm_sllv_epi32(mant, _mm_sub_epi32(_mm_setzero_si128(), d)));
    return _mm_andnot_si128(ez, r);
}
XO V cfltu_v(V x, int i8) {
    V E;
    V r = cvt_mag(x, i8, E);
    r = _mm_or_si128(r, _mm_cmpgt_epi32(E, s32(31)));
    return _mm_andnot_si128(_mm_srai_epi32(x, 31), r);
}
XO V cflts_v(V x, int i8) {
    V E;
    V r = cvt_mag(x, i8, E);
    V neg = _mm_srai_epi32(x, 31);
    r = _mm_sub_epi32(_mm_xor_si128(r, neg), neg);
    V sat = _mm_xor_si128(s32(0x7FFFFFFF), neg);
    return _mm_blendv_epi8(r, sat, _mm_cmpgt_epi32(E, s32(30)));
}
XO V ifscale(V x, int i8) { return I(_mm_mul_ps(_mm_cvtepi32_ps(x), F(s32(uint32_t(127 + i8 - 155) << 23)))); }
inline V cflts_scalar(V x, int i8) {
    alignas(16) uint32_t a[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(a), x);
    for (auto& v : a) v = uint32_t(spuf::cflts(v, i8));
    return _mm_load_si128(reinterpret_cast<const __m128i*>(a));
}
inline V cfltu_scalar(V x, int i8) {
    alignas(16) uint32_t a[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(a), x);
    for (auto& v : a) v = spuf::cfltu(v, i8);
    return _mm_load_si128(reinterpret_cast<const __m128i*>(a));
}
inline V cuflt_scalar(V x, int i8) {
    alignas(16) uint32_t a[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(a), x);
    for (auto& v : a) v = spuf::cuflt(v, i8);
    return _mm_load_si128(reinterpret_cast<const __m128i*>(a));
}
inline V csflt_scalar(V x, int i8) {
    alignas(16) uint32_t a[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(a), x);
    for (auto& v : a) v = spuf::csflt(int32_t(v), i8);
    return _mm_load_si128(reinterpret_cast<const __m128i*>(a));
}
XO V cflts_slow(V x, int i8) {
    if (spuf::avx2_on()) return cflts_v(x, i8);
    return cflts_scalar(x, i8);
}
XO V cfltu(V x, int i8) {
    if (spuf::avx2_on()) return cfltu_v(x, i8);
    return cfltu_scalar(x, i8);
}
XO V cuflt_slow(V x, int i8) {
    if (spuf::avx2_on() && i8 >= 30 && i8 <= 258 && _mm_testz_si128(x, s32(0xff000000u))) return ifscale(x, i8);
    return cuflt_scalar(x, i8);
}
XO V csflt(V x, int i8) {
    if (spuf::avx2_on() && i8 >= 30 && i8 <= 258 && _mm_testz_si128(_mm_add_epi32(x, s32(0x1000000)), s32(0xfe000000u))) return ifscale(x, i8);
    return csflt_scalar(x, i8);
}
XO V cflts(V x, int i8) {
    if (i8 != 173) return cflts_slow(x, i8);
    __m128i r = _mm_cvttps_epi32(F(x));
    __m128i big = _mm_cmpgt_epi32(_mm_and_si128(x, s32(0x7FFFFFFF)), s32(0x4EFFFFFF));
    __m128i sat = _mm_xor_si128(s32(0x7FFFFFFF), _mm_srai_epi32(x, 31));
    return _mm_blendv_epi8(r, sat, big);
}
XO V cuflt(V x, int i8) {
    if (i8 != 155 || _mm_movemask_ps(F(x))) return cuflt_slow(x, i8);
    return I(_mm_cvtepi32_ps(x));
}

#undef XO

}
