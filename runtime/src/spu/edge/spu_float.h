#pragma once

#include <immintrin.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace spuf {

struct CsrGuard {
    unsigned old;
    CsrGuard() : old(_mm_getcsr()) { _mm_setcsr((old & ~0x6000u) | 0x6000u | 0x8040u); }
    ~CsrGuard() { _mm_setcsr(old); }
};

inline uint32_t bits(float f) { return std::bit_cast<uint32_t>(f); }
inline float fl(uint32_t u) { return std::bit_cast<float>(u); }

inline double to_double(uint32_t b) {
    uint32_t e = (b >> 23) & 0xFF;
    if (!e) return 0.0;
    uint64_t d = (uint64_t(b >> 31) << 63) | (uint64_t(e - 127 + 1023) << 52) | (uint64_t(b & 0x7FFFFF) << 29);
    return std::bit_cast<double>(d);
}

inline uint32_t from_double(double v) {
    uint64_t d = std::bit_cast<uint64_t>(v);
    uint32_t sign = uint32_t(d >> 63) << 31;
    int32_t e = int32_t((d >> 52) & 0x7FF);
    if (e == 0) return 0;
    if (e == 0x7FF) return sign | 0x7FFFFFFFu;
    int32_t se = e - 1023 + 127;
    if (se <= 0) return 0;
    if (se > 255) return sign | 0x7FFFFFFFu;
    return sign | (uint32_t(se) << 23) | uint32_t((d >> 29) & 0x7FFFFF);
}

inline __m128i special(__m128 r) {
    __m128 mag = _mm_and_ps(r, _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)));
    return _mm_castps_si128(_mm_cmp_ps(mag, _mm_castsi128_ps(_mm_set1_epi32(0x7F7FFFFF)), _CMP_NLT_UQ));
}

inline __m128 pos_zero(__m128 r) { return _mm_andnot_ps(_mm_cmpeq_ps(r, _mm_setzero_ps()), r); }

template <class F>
__attribute__((noinline, cold)) inline __m128 __vectorcall slow(__m128 a, __m128 b, __m128 c, __m128 r, __m128i lanes, F f) {
    alignas(16) uint32_t av[4], bv[4], cv[4], rv[4], lv[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(av), _mm_castps_si128(a));
    _mm_store_si128(reinterpret_cast<__m128i*>(bv), _mm_castps_si128(b));
    _mm_store_si128(reinterpret_cast<__m128i*>(cv), _mm_castps_si128(c));
    _mm_store_si128(reinterpret_cast<__m128i*>(rv), _mm_castps_si128(r));
    _mm_store_si128(reinterpret_cast<__m128i*>(lv), lanes);
    for (int i = 0; i < 4; i++)
        if (lv[i]) rv[i] = from_double(f(to_double(av[i]), to_double(bv[i]), to_double(cv[i])));
    return _mm_castsi128_ps(_mm_load_si128(reinterpret_cast<const __m128i*>(rv)));
}

struct AddD {
    double operator()(double x, double y, double) const { return x + y; }
};
struct SubD {
    double operator()(double x, double y, double) const { return x - y; }
};
struct MulD {
    double operator()(double x, double y, double) const { return x * y; }
};
struct MaD {
    double operator()(double x, double y, double z) const { return std::fma(x, y, z); }
};
struct MsD {
    double operator()(double x, double y, double z) const { return std::fma(x, y, -z); }
};
struct NmsD {
    double operator()(double x, double y, double z) const { return std::fma(-x, y, z); }
};

template <class Slow>
inline __m128 fix(__m128 a, __m128 b, __m128 c, __m128 r, Slow s) {
    __m128i bad = special(r);
    if (!_mm_testz_si128(bad, bad)) r = slow(a, b, c, r, bad, s);
    return pos_zero(r);
}

inline __m128 fa(__m128 a, __m128 b) { return fix(a, b, a, _mm_add_ps(a, b), AddD{}); }
inline __m128 fs(__m128 a, __m128 b) { return fix(a, b, a, _mm_sub_ps(a, b), SubD{}); }
inline __m128 fm(__m128 a, __m128 b) { return fix(a, b, a, _mm_mul_ps(a, b), MulD{}); }
inline __m128 fma(__m128 a, __m128 b, __m128 c) { return fix(a, b, c, _mm_fmadd_ps(a, b, c), MaD{}); }
inline __m128 fms(__m128 a, __m128 b, __m128 c) { return fix(a, b, c, _mm_fmsub_ps(a, b, c), MsD{}); }
inline __m128 fnms(__m128 a, __m128 b, __m128 c) { return fix(a, b, c, _mm_fnmadd_ps(a, b, c), NmsD{}); }

inline float fa(float a, float b) { return _mm_cvtss_f32(fa(_mm_set_ss(a), _mm_set_ss(b))); }
inline float fs(float a, float b) { return _mm_cvtss_f32(fs(_mm_set_ss(a), _mm_set_ss(b))); }
inline float fm(float a, float b) { return _mm_cvtss_f32(fm(_mm_set_ss(a), _mm_set_ss(b))); }
inline float fma(float a, float b, float c) { return _mm_cvtss_f32(fma(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(c))); }
inline float fms(float a, float b, float c) { return _mm_cvtss_f32(fms(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(c))); }
inline float fnms(float a, float b, float c) { return _mm_cvtss_f32(fnms(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(c))); }

inline uint32_t smax_fix(uint32_t b) {
    if ((b & 0x7F800000) == 0x7F800000) return (b & 0x80000000) | 0x7FFFFFFF;
    return b;
}

inline float frest(float x) {
    uint32_t b = bits(x);
    if ((b & 0x7F800000) == 0) return fl((b & 0x80000000) | 0x7FFFFFFF);
    return fl(smax_fix(bits(1.0f / x)));
}

inline float frsqest(float x) {
    uint32_t b = bits(x) & 0x7FFFFFFF;
    if ((b & 0x7F800000) == 0) return fl(0x7FFFFFFF);
    return fl(smax_fix(bits(1.0f / std::sqrt(fl(b)))));
}

inline __m128 frest(__m128 x) {
    alignas(16) float v[4];
    _mm_store_ps(v, x);
    for (float& f : v) f = frest(f);
    return _mm_load_ps(v);
}

inline __m128 frsqest(__m128 x) {
    alignas(16) float v[4];
    _mm_store_ps(v, x);
    for (float& f : v) f = frsqest(f);
    return _mm_load_ps(v);
}

inline uint32_t cuflt(uint32_t u, int i8) { return from_double(double(u) * std::bit_cast<double>(uint64_t(1023 + i8 - 155) << 52)); }
inline uint32_t csflt(int32_t s, int i8) { return from_double(double(s) * std::bit_cast<double>(uint64_t(1023 + i8 - 155) << 52)); }

inline int32_t cflts(uint32_t f, int i8) {
    double v = to_double(f) * std::bit_cast<double>(uint64_t(1023 + 173 - i8) << 52);
    return v >= 2147483647.0 ? INT32_MAX : v <= -2147483648.0 ? INT32_MIN : int32_t(v);
}

inline uint32_t cfltu(uint32_t f, int i8) {
    double v = to_double(f) * std::bit_cast<double>(uint64_t(1023 + 173 - i8) << 52);
    return v >= 4294967295.0 ? UINT32_MAX : v <= 0.0 ? 0u : uint32_t(v);
}

inline int32_t key(uint32_t v) {
    uint32_t mag = v & 0x7FFFFFFF;
    if ((v & 0x7F800000) == 0) mag = 0;
    return (v & 0x80000000) ? -int32_t(mag) : int32_t(mag);
}

inline bool fcgt(float a, float b) { return key(bits(a)) > key(bits(b)); }
inline bool fceq(float a, float b) { return key(bits(a)) == key(bits(b)); }

inline uint32_t mag(uint32_t v) { return (v & 0x7F800000) == 0 ? 0 : (v & 0x7FFFFFFF); }
inline bool fcmgt(float a, float b) { return mag(bits(a)) > mag(bits(b)); }

inline __m128 ld4(const uint8_t* p) { return _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)), _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12))); }
inline void st4(uint8_t* p, __m128 v) { _mm_storeu_si128(reinterpret_cast<__m128i*>(p), _mm_shuffle_epi8(_mm_castps_si128(v), _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12))); }

inline float ldf(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return fl(__builtin_bswap32(v));
}
inline void stf(uint8_t* p, float f) {
    uint32_t v = __builtin_bswap32(bits(f));
    std::memcpy(p, &v, 4);
}
inline uint32_t ld32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}
inline void st32(uint8_t* p, uint32_t x) {
    uint32_t v = __builtin_bswap32(x);
    std::memcpy(p, &v, 4);
}
inline uint32_t ld16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }
inline void st16(uint8_t* p, uint32_t x) {
    p[0] = uint8_t(x >> 8);
    p[1] = uint8_t(x);
}

}
