#include "edge_vcomp.h"
#include "spu_float.h"

#include <cstring>

namespace edge {

namespace {

constexpr uint32_t kMask = 0x3ffff;

inline uint32_t rd32(const uint8_t* ls, uint32_t a) { return spuf::ld32(ls + (a & 0x3fffc)); }

uint32_t slot_of(const uint8_t* ls, uint32_t ctx, uint8_t attr) {
    for (uint32_t i = 0; i < 16; i++)
        if (ls[ctx + 352 + i] == attr) return i;
    return 16;
}

uint32_t table_of(const uint8_t* ls, uint32_t ctx, uint8_t attr) { return rd32(ls, ctx + 16 + 4 * slot_of(ls, ctx, attr)); }

enum Enc { F32X3, S16X4, P111110, F16X3 };

struct Field {
    uint8_t attr;
    Enc enc;
};

struct Format {
    int id;
    uint32_t rec;
    uint32_t nq;
    int nfields;
    Field f[8];
};

const Format kFormats[] = {
    {0, 12, 2, 1, {{1, F32X3}}},
    {7, 36, 5, 4, {{1, F32X3}, {3, S16X4}, {4, P111110}, {31, F32X3}}},
    {4, 54, 7, 7, {{1, F32X3}, {3, S16X4}, {4, P111110}, {27, F32X3}, {28, F16X3}, {29, F16X3}, {30, F16X3}}},
    {3, 46, 6, 6, {{1, F32X3}, {2, P111110}, {27, F32X3}, {28, F16X3}, {29, F16X3}, {30, F16X3}}},
    {6, 28, 4, 3, {{1, F32X3}, {2, P111110}, {31, F32X3}}},
};

inline uint32_t enc_s16(uint32_t x) {
    float v = spuf::fma(spuf::fl(x), 32767.5f, -0.5f);
    return uint32_t(spuf::cflts(spuf::bits(v), 157)) >> 16;
}

inline uint32_t enc_p111110(uint32_t x, uint32_t y, uint32_t z) {
    uint32_t X = uint32_t(spuf::cflts(spuf::bits(spuf::fma(spuf::fl(x), spuf::fl(0x3f7fe000), spuf::fl(0xba000000))), 142));
    uint32_t Y = uint32_t(spuf::cflts(spuf::bits(spuf::fma(spuf::fl(y), spuf::fl(0x3f7fe000), spuf::fl(0xba000000))), 142));
    uint32_t Z = uint32_t(spuf::cflts(spuf::bits(spuf::fma(spuf::fl(z), spuf::fl(0x3f7fc000), spuf::fl(0xba800000))), 142));
    return (Z & 0xffc00000u) | ((Y >> 21) << 11) | (X >> 21);
}

inline uint32_t enc_f16(uint32_t b) {
    uint32_t m = b & 0x7fffffffu;
    if (m < 0x38000000u) return 0;
    uint32_t h = m >= 0x47800000u ? 0x7bffu : ((m - 0x38000000u) >> 13) & 0x7fffu;
    return h | ((b >> 16) & 0x8000u);
}

void encode_vertex(const uint8_t* ls, const Format& fm, const uint32_t* tabs, uint32_t v, uint8_t* o) {
    for (int i = 0; i < fm.nfields; i++) {
        const uint8_t* src = ls + ((tabs[i] + 16 * v) & 0x3fff0);
        uint32_t x = spuf::ld32(src), y = spuf::ld32(src + 4), z = spuf::ld32(src + 8), w = spuf::ld32(src + 12);
        switch (fm.f[i].enc) {
        case F32X3:
            std::memcpy(o, src, 12);
            o += 12;
            break;
        case S16X4:
            spuf::st16(o, enc_s16(x));
            spuf::st16(o + 2, enc_s16(y));
            spuf::st16(o + 4, enc_s16(z));
            spuf::st16(o + 6, enc_s16(w));
            o += 8;
            break;
        case P111110:
            spuf::st32(o, enc_p111110(x, y, z));
            o += 4;
            break;
        case F16X3:
            spuf::st16(o, enc_f16(x));
            spuf::st16(o + 2, enc_f16(y));
            spuf::st16(o + 4, enc_f16(z));
            o += 6;
            break;
        }
    }
}

inline __m128i cflts_v(__m128 v, int pw) {
    __m128i r = _mm_cvttps_epi32(_mm_mul_ps(v, _mm_castsi128_ps(_mm_set1_epi32((127 + pw) << 23))));
    __m128i fix = _mm_andnot_si128(_mm_srai_epi32(_mm_castps_si128(v), 31), _mm_cmpeq_epi32(r, _mm_set1_epi32(int(0x80000000))));
    return _mm_xor_si128(r, fix);
}

inline __m256 ld8(const uint8_t* p0, const uint8_t* p1) {
    return _mm256_set_m128(spuf::ld4(p1), spuf::ld4(p0));
}

inline __m256 fma8(__m256 a, __m256 b, __m256 c) {
    __m256 r = _mm256_fmadd_ps(a, b, c);
    __m256 mag = _mm256_and_ps(r, _mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff)));
    __m256 bad = _mm256_cmp_ps(mag, _mm256_castsi256_ps(_mm256_set1_epi32(0x7f7fffff)), _CMP_NLT_UQ);
    if (!_mm256_testz_ps(bad, bad)) {
        __m128 lo = spuf::fma(_mm256_castps256_ps128(a), _mm256_castps256_ps128(b), _mm256_castps256_ps128(c));
        __m128 hi = spuf::fma(_mm256_extractf128_ps(a, 1), _mm256_extractf128_ps(b, 1), _mm256_extractf128_ps(c, 1));
        return _mm256_set_m128(hi, lo);
    }
    return _mm256_andnot_ps(_mm256_cmp_ps(r, _mm256_setzero_ps(), _CMP_EQ_OQ), r);
}

inline __m256i cflts8(__m256 v, int pw) {
    __m256i r = _mm256_cvttps_epi32(_mm256_mul_ps(v, _mm256_castsi256_ps(_mm256_set1_epi32((127 + pw) << 23))));
    __m256i fix = _mm256_andnot_si256(_mm256_srai_epi32(_mm256_castps_si256(v), 31), _mm256_cmpeq_epi32(r, _mm256_set1_epi32(int(0x80000000))));
    return _mm256_xor_si256(r, fix);
}

void encode_columns(uint8_t* ls, const Format& fm, const uint32_t* tabs, uint32_t n2, uint8_t* out) {
    const __m128i be16 = _mm_setr_epi8(3, 2, 7, 6, 11, 10, 15, 14, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m128i be16h = _mm_setr_epi8(1, 0, 5, 4, 9, 8, 13, 12, -1, -1, -1, -1, -1, -1, -1, -1);
    const uint32_t rec = fm.rec;
    uint32_t off = 0;
    for (int i = 0; i < fm.nfields; i++) {
        const uint8_t* src = ls + tabs[i];
        uint8_t* o = out + off;
        switch (fm.f[i].enc) {
        case F32X3:
            if (i + 1 < fm.nfields)
                for (uint32_t v = 0; v < n2; v++) _mm_storeu_si128(reinterpret_cast<__m128i*>(o + rec * v), _mm_load_si128(reinterpret_cast<const __m128i*>(src + 16 * v)));
            else
                for (uint32_t v = 0; v < n2; v++) {
                    _mm_storel_epi64(reinterpret_cast<__m128i*>(o + rec * v), _mm_loadl_epi64(reinterpret_cast<const __m128i*>(src + 16 * v)));
                    std::memcpy(o + rec * v + 8, src + 16 * v + 8, 4);
                }
            off += 12;
            break;
        case S16X4: {
            const __m128 m = _mm_set1_ps(32767.5f), c = _mm_set1_ps(-0.5f);
            const __m256i sh8 = _mm256_setr_epi8(3, 2, 7, 6, 11, 10, 15, 14, -1, -1, -1, -1, -1, -1, -1, -1, 3, 2, 7, 6, 11, 10, 15, 14, -1, -1, -1, -1, -1, -1, -1, -1);
            uint32_t v = 0;
            for (; v + 1 < n2; v += 2) {
                __m256 f = fma8(ld8(src + 16 * v, src + 16 * v + 16), _mm256_set1_ps(32767.5f), _mm256_set1_ps(-0.5f));
                __m256i r = _mm256_shuffle_epi8(cflts8(f, 16), sh8);
                _mm_storel_epi64(reinterpret_cast<__m128i*>(o + rec * v), _mm256_castsi256_si128(r));
                _mm_storel_epi64(reinterpret_cast<__m128i*>(o + rec * v + rec), _mm256_extracti128_si256(r, 1));
            }
            for (; v < n2; v++) {
                __m128 f = spuf::fma(spuf::ld4(src + 16 * v), m, c);
                _mm_storel_epi64(reinterpret_cast<__m128i*>(o + rec * v), _mm_shuffle_epi8(cflts_v(f, 16), be16));
            }
            off += 8;
            break;
        }
        case P111110: {
            const __m128 m = _mm_castsi128_ps(_mm_setr_epi32(0x3f7fe000, 0x3f7fe000, 0x3f7fc000, 0));
            const __m128 c = _mm_castsi128_ps(_mm_setr_epi32(int(0xba000000), int(0xba000000), int(0xba800000), 0));
            const __m256 m8 = _mm256_set_m128(m, m), c8 = _mm256_set_m128(c, c);
            uint32_t v = 0;
            for (; v + 1 < n2; v += 2) {
                __m256i q = cflts8(fma8(ld8(src + 16 * v, src + 16 * v + 16), m8, c8), 31);
                __m256i x = _mm256_srli_epi32(q, 21);
                __m256i y = _mm256_slli_epi32(x, 11);
                __m256i z = _mm256_and_si256(q, _mm256_set1_epi32(int(0xffc00000)));
                __m256i w = _mm256_or_si256(_mm256_or_si256(_mm256_shuffle_epi32(z, 0xaa), _mm256_shuffle_epi32(y, 0x55)), x);
                spuf::st32(o + rec * v, uint32_t(_mm256_extract_epi32(w, 0)));
                spuf::st32(o + rec * v + rec, uint32_t(_mm256_extract_epi32(w, 4)));
            }
            for (; v < n2; v++) {
                __m128i q = cflts_v(spuf::fma(spuf::ld4(src + 16 * v), m, c), 31);
                __m128i x = _mm_srli_epi32(q, 21);
                __m128i y = _mm_slli_epi32(x, 11);
                __m128i z = _mm_and_si128(q, _mm_set1_epi32(int(0xffc00000)));
                uint32_t w = uint32_t(_mm_extract_epi32(z, 2)) | uint32_t(_mm_extract_epi32(y, 1)) | uint32_t(_mm_cvtsi128_si32(x));
                spuf::st32(o + rec * v, w);
            }
            off += 4;
            break;
        }
        case F16X3: {
            uint32_t v = 0;
            bool last = i + 1 == fm.nfields;
            {
                const __m256i sh = _mm256_setr_epi8(1, 0, 5, 4, 9, 8, 13, 12, -1, -1, -1, -1, -1, -1, -1, -1, 1, 0, 5, 4, 9, 8, 13, 12, -1, -1, -1, -1, -1, -1, -1, -1);
                const __m256i bswap = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
                for (; v + 1 < n2; v += 2) {
                    __m256i b = _mm256_shuffle_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + 16 * v)), bswap);
                    __m256i m = _mm256_and_si256(b, _mm256_set1_epi32(0x7fffffff));
                    __m256i h = _mm256_and_si256(_mm256_srli_epi32(_mm256_sub_epi32(m, _mm256_set1_epi32(0x38000000)), 13), _mm256_set1_epi32(0x7fff));
                    h = _mm256_blendv_epi8(h, _mm256_set1_epi32(0x7bff), _mm256_cmpgt_epi32(m, _mm256_set1_epi32(0x477fffff)));
                    h = _mm256_or_si256(h, _mm256_and_si256(_mm256_srli_epi32(b, 16), _mm256_set1_epi32(0x8000)));
                    h = _mm256_andnot_si256(_mm256_cmpgt_epi32(_mm256_set1_epi32(0x38000000), m), h);
                    __m256i r = _mm256_shuffle_epi8(h, sh);
                    if (last) {
                        uint64_t a0 = uint64_t(_mm256_extract_epi64(r, 0)), a1 = uint64_t(_mm256_extract_epi64(r, 2));
                        std::memcpy(o + rec * v, &a0, 6);
                        std::memcpy(o + rec * v + rec, &a1, 6);
                    } else {
                        _mm_storel_epi64(reinterpret_cast<__m128i*>(o + rec * v), _mm256_castsi256_si128(r));
                        _mm_storel_epi64(reinterpret_cast<__m128i*>(o + rec * v + rec), _mm256_extracti128_si256(r, 1));
                    }
                }
            }
            for (; v < n2; v++) {
                __m128i b = _mm_castps_si128(spuf::ld4(src + 16 * v));
                __m128i m = _mm_and_si128(b, _mm_set1_epi32(0x7fffffff));
                __m128i h = _mm_and_si128(_mm_srli_epi32(_mm_sub_epi32(m, _mm_set1_epi32(0x38000000)), 13), _mm_set1_epi32(0x7fff));
                h = _mm_blendv_epi8(h, _mm_set1_epi32(0x7bff), _mm_cmpgt_epi32(m, _mm_set1_epi32(0x477fffff)));
                h = _mm_or_si128(h, _mm_and_si128(_mm_srli_epi32(b, 16), _mm_set1_epi32(0x8000)));
                h = _mm_andnot_si128(_mm_cmplt_epi32(m, _mm_set1_epi32(0x38000000)), h);
                __m128i r = _mm_shuffle_epi8(h, be16h);
                if (i + 1 < fm.nfields) _mm_storel_epi64(reinterpret_cast<__m128i*>(o + rec * v), r);
                else {
                    uint32_t lo = uint32_t(_mm_cvtsi128_si32(r));
                    uint16_t hi = uint16_t(_mm_extract_epi16(r, 2));
                    std::memcpy(o + rec * v, &lo, 4);
                    std::memcpy(o + rec * v + 4, &hi, 2);
                }
            }
            off += 6;
            break;
        }
        }
    }
}

void compress_fmt(uint8_t* ls, uint32_t ctx, uint32_t nv, uint32_t out, const Format& fm) {
    uint32_t tabs[8];
    for (int i = 0; i < fm.nfields; i++) tabs[i] = table_of(ls, ctx, fm.f[i].attr);
    uint32_t n2 = (nv + 1) & ~1u;
    uint32_t pairs = n2 / 2;
    uint32_t pr = 2 * fm.rec;
    bool fast = uint64_t(out) + uint64_t(pr) * pairs + 16 * fm.nq + 16 <= 0x40000;
    for (int i = 0; i < fm.nfields && fast; i++) fast = (tabs[i] & 15) == 0 && uint64_t(tabs[i]) + 16ull * n2 <= 0x40000;
    if (fast && pairs) {
        encode_columns(ls, fm, tabs, n2, ls + out);
        uint32_t A = out + pr * (pairs - 1);
        std::memset(ls + A + pr, 0, 16 * fm.nq - pr);
        uint32_t a = A & 15;
        std::memcpy(ls + A + 16 * fm.nq, ls + A + 16 * (fm.nq - 1), 16 - a);
        return;
    }
    uint8_t d[128];
    for (uint32_t k = 0; k < pairs; k++) {
        std::memset(d, 0, sizeof d);
        encode_vertex(ls, fm, tabs, 2 * k, d);
        encode_vertex(ls, fm, tabs, 2 * k + 1, d + fm.rec);
        uint32_t A = out + pr * k;
        uint32_t len = k + 1 == pairs ? 16 * fm.nq : pr;
        for (uint32_t i = 0; i < len; i++) ls[(A + i) & kMask] = d[i];
        if (k + 1 == pairs) {
            uint32_t a = A & 15;
            uint32_t base = 16 * (fm.nq - 1);
            for (uint32_t i = 0; i < 16 - a; i++) ls[(A + 16 * fm.nq + i) & kMask] = d[base + i];
        }
    }
}

}

bool compress_vertexes(uint8_t* ls, uint32_t ctx, int overlay, uint32_t r3out[4], Outputs* outs, bool* called, uint32_t limit) {
    spuf::CsrGuard csr;
    if (called) *called = false;
    uint32_t w80 = rd32(ls, ctx + 80), w84 = rd32(ls, ctx + 84), w88 = rd32(ls, ctx + 88), w92 = rd32(ls, ctx + 92);
    if (ls[ctx] & 0x10) {
        r3out[0] = r3out[1] = r3out[2] = r3out[3] = 0;
        return true;
    }
    uint32_t cb = rd32(ls, ctx + 140);
    uint32_t nv = spuf::ld16(ls + ctx + 8);
    uint32_t stride_cb = rd32(ls, ctx + 136);
    if (nv && stride_cb) return false;
    const Format* fm = nullptr;
    for (const Format& f : kFormats)
        if (f.id == overlay) fm = &f;
    if (nv && cb && !fm) return false;
    if (ctx < 0x10000 || ctx + 0x200 > 0x40000) return false;
    if (nv && cb) {
        uint32_t n2 = (nv + 1) & ~1u;
        uint64_t o = w88;
        if (o < 0x10c80 || o + uint64_t(2 * fm->rec) * (n2 / 2) + 16 * fm->nq + 32 > limit) return false;
        for (int i = 0; i < fm->nfields; i++) {
            uint64_t t = table_of(ls, ctx, fm->f[i].attr);
            if ((t & 15) || t < 0x10c80 || t + 16ull * n2 + 16 > limit) return false;
            uint64_t oe = o + uint64_t(2 * fm->rec) * (n2 / 2) + 16 * fm->nq + 32;
            if (t < oe && o < t + 16ull * n2 + 16) return false;
        }
    }
    r3out[0] = w88;
    r3out[1] = w92;
    r3out[2] = w80;
    r3out[3] = w84;
    spuf::st32(ls + ctx + 84, w88);
    if (outs) outs->add(ctx + 84, ctx + 88);
    if (!nv) return true;
    uint32_t out = w88 & kMask;
    if (cb) {
        compress_fmt(ls, ctx, nv, out, *fm);
        if (called) *called = true;
    }
    uint32_t p = rd32(ls, ctx + 116);
    uint32_t stride = p ? ls[(p + 1) & kMask] : 0;
    uint32_t size = ((stride & 0xffff) * (nv & 0xffff) + 15) & ~15u;
    uint32_t cur = rd32(ls, ctx + 88);
    spuf::st32(ls + ctx + 88, cur + size);
    if (outs) {
        outs->add(ctx + 88, ctx + 92);
        outs->add(out, out + stride * nv);
    }
    return true;
}

void compress_vertexes_stack(uint8_t* ls, const uint32_t regs[128][4], int overlay, bool called, const uint32_t r3out[4]) {
    uint32_t sp = regs[1][0] & 0x3fff0;
    auto put = [&](uint32_t a, const uint32_t* q) {
        for (int i = 0; i < 4; i++) spuf::st32(ls + ((a + 4 * i) & 0x3fffc), q[i]);
    };
    put(sp + 16, regs[0]);
    put(sp - 16, regs[80]);
    put(sp - 32, regs[81]);
    put(sp - 48, regs[82]);
    put(sp - 80, regs[1]);
    if (!called || (overlay != 3 && overlay != 4)) return;
    uint32_t osp = sp - 80;
    uint32_t last = overlay == 4 ? 117 : 100;
    for (uint32_t n = 80; n <= last; n++) {
        const uint32_t* q = n == 81 ? regs[3] : n == 82 ? r3out : regs[n];
        put(osp - 16 * (n - 79), q);
    }
    put(osp - (overlay == 4 ? 624 : 352), regs[126]);
    uint32_t r1[4];
    for (int i = 0; i < 4; i++) r1[i] = regs[1][i] - 80;
    put(osp - (overlay == 4 ? 656 : 384), r1);
}

}
