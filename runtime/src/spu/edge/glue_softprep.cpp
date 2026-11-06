#include "edge_glue.h"

#include "spu_float.h"

#include <cstring>

namespace edge::glue {

namespace {

constexpr uint32_t kQuadMask = 0x3fff0;
constexpr uint32_t kDeformThresholdLs = 0x10b90;
constexpr uint32_t kDeformMatrixA = 60;
constexpr uint32_t kDeformMatrixB = 96;

inline __m128 ldq(const uint8_t* ls, uint32_t a) { return spuf::ld4(ls + (a & kQuadMask)); }

inline void stq(uint8_t* ls, uint32_t a, __m128 v) { spuf::st4(ls + (a & kQuadMask), v); }

inline __m128 column(const uint8_t* ls, uint32_t base, uint32_t j) {
    return _mm_setr_ps(spuf::ldf(ls + ((base + 4 * j) & kLsMask)), spuf::ldf(ls + ((base + 12 + 4 * j) & kLsMask)), spuf::ldf(ls + ((base + 24 + 4 * j) & kLsMask)), 0.0f);
}

inline __m128 fcgt4(__m128 a, __m128 b) {
    alignas(16) float x[4], y[4];
    alignas(16) uint32_t r[4];
    _mm_store_ps(x, a);
    _mm_store_ps(y, b);
    for (int i = 0; i < 4; i++) r[i] = spuf::fcgt(x[i], y[i]) ? 0xffffffffu : 0;
    return _mm_load_ps(reinterpret_cast<const float*>(r));
}

inline float lane(__m128 v, int i) {
    alignas(16) float t[4];
    _mm_store_ps(t, v);
    return t[i];
}

inline float hsum3(__m128 p) { return spuf::fa(spuf::fa(lane(p, 0), lane(p, 1)), lane(p, 2)); }

}

bool softbody_prepare(uint8_t* ls, uint32_t blk, uint32_t limit, uint32_t disable) {
    uint32_t hdr = blk & kQuadMask;
    uint32_t off1 = rd32(ls, hdr + 4);
    uint32_t off3 = rd32(ls, hdr + 12);
    uint32_t n = rd32(ls, hdr + 16) + rd32(ls, hdr + 20) + rd32(ls, hdr + 24) + rd32(ls, hdr + 28);
    if ((disable & 0xff) != 0 && int32_t(n) > 0) {
        for (uint32_t k = 0; k < n; k++)
            if (ls[(blk + off3 + k) & kLsMask] & 0x80) std::memset(ls + ((blk + off1 + 16 * k) & kQuadMask), 0, 16);
    }
    return int32_t(n) <= int32_t(limit);
}

bool softprep_9a60(uint8_t* ls, Mfc&, uint32_t r3, uint32_t, uint32_t, uint32_t r6, uint32_t r7, uint32_t, uint32_t& r3_out) {
    if (!softbody_prepare(ls, r3, r6, r7)) return false;
    r3_out = r3;
    return true;
}

void asym_deform(uint8_t* ls, uint32_t out, uint32_t in, uint32_t count, uint32_t blk, const uint32_t scale[4]) {
    if (int32_t(count) <= 0) return;
    spuf::CsrGuard guard;
    __m128 ca[3], cb[3];
    for (uint32_t j = 0; j < 3; j++) {
        ca[j] = column(ls, blk + kDeformMatrixA, j);
        cb[j] = column(ls, blk + kDeformMatrixB, j);
    }
    __m128 sc = _mm_setr_ps(spuf::fl(scale[0]), spuf::fl(scale[1]), spuf::fl(scale[2]), spuf::fl(scale[3]));
    __m128 sign = _mm_castsi128_ps(_mm_set1_epi32(int(0x80000000u)));
    for (uint32_t g = 0; g < count; g += 4) {
        __m128 thr = ldq(ls, kDeformThresholdLs);
        __m128 x[4], o[4];
        for (int i = 0; i < 4; i++) x[i] = ldq(ls, in + 16 * (g + i));
        for (int i = 0; i < 4; i++) o[i] = ldq(ls, out + 16 * (g + i));
        for (int i = 0; i < 4; i++) {
            __m128 mask = fcgt4(thr, x[i]);
            __m128 ax = _mm_andnot_ps(sign, x[i]);
            float d[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (int j = 0; j < 3; j++) {
                __m128 col = _mm_blendv_ps(ca[j], cb[j], mask);
                d[j] = hsum3(spuf::fm(col, ax));
            }
            o[i] = spuf::fma(sc, _mm_setr_ps(d[0], d[1], d[2], 0.0f), o[i]);
        }
        for (int i = 0; i < 4; i++) stq(ls, out + 16 * (g + i), o[i]);
    }
}

}
