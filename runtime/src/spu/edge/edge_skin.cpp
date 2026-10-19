#include "edge_skin.h"
#include "spu_float.h"

#include <vector>

namespace edge {

namespace {

constexpr uint32_t kQMask = 0x3fff0;

struct Ls {
    uint8_t* p;
    __m128 q(uint32_t a) const { return spuf::ld4(p + (a & kQMask)); }
    void st(uint32_t a, __m128 v) const { spuf::st4(p + (a & kQMask), v); }
    uint32_t w(uint32_t a) const { return spuf::ld32(p + (a & 0x3fffc)); }
};

inline __m128 yzx(__m128 v) { return _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 0, 2, 1)); }
inline __m128 zxy(__m128 v) { return _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 1, 0, 2)); }

const __m256i kBswap = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);

inline __m256 ld8(const uint8_t* ls, uint32_t lo, uint32_t hi) {
    __m256i v = _mm256_loadu2_m128i(reinterpret_cast<const __m128i*>(ls + (hi & kQMask)), reinterpret_cast<const __m128i*>(ls + (lo & kQMask)));
    return _mm256_castsi256_ps(_mm256_shuffle_epi8(v, kBswap));
}

inline void st8(uint8_t* ls, uint32_t lo, uint32_t hi, __m256 v) {
    __m256i b = _mm256_shuffle_epi8(_mm256_castps_si256(v), kBswap);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(ls + (lo & kQMask)), _mm256_castsi256_si128(b));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(ls + (hi & kQMask)), _mm256_extracti128_si256(b, 1));
}

struct Fast {
    __m256i bad = _mm256_setzero_si256();
    __m256 chk(__m256 r) {
        __m256 mag = _mm256_and_ps(r, _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF)));
        bad = _mm256_or_si256(bad, _mm256_castps_si256(_mm256_cmp_ps(mag, _mm256_castsi256_ps(_mm256_set1_epi32(0x7F7FFFFF)), _CMP_NLT_UQ)));
        return _mm256_andnot_ps(_mm256_cmp_ps(r, _mm256_setzero_ps(), _CMP_EQ_OQ), r);
    }
    __m256 fm(__m256 a, __m256 b) { return chk(_mm256_mul_ps(a, b)); }
    __m256 fma(__m256 a, __m256 b, __m256 c) { return chk(_mm256_fmadd_ps(a, b, c)); }
    bool ok() const { return _mm256_testz_si256(bad, bad); }
};

inline __m256 yzx8(__m256 v) { return _mm256_permute_ps(v, _MM_SHUFFLE(3, 0, 2, 1)); }
inline __m256 zxy8(__m256 v) { return _mm256_permute_ps(v, _MM_SHUFFLE(3, 1, 0, 2)); }

void cofactor_one(const Ls& ls, uint32_t t, uint32_t a) {
    __m128 A = ls.q(t), B = ls.q(t + 16), C = ls.q(t + 32), n = ls.q(a);
    __m128 m0 = spuf::fms(yzx(A), zxy(A), spuf::fm(zxy(C), yzx(B)));
    __m128 m1 = spuf::fms(zxy(B), yzx(B), spuf::fm(yzx(C), zxy(A)));
    __m128 m2 = spuf::fms(yzx(C), zxy(C), spuf::fm(zxy(B), yzx(A)));
    __m128 o = spuf::fma(m2, zxy(n), spuf::fma(m1, yzx(n), spuf::fm(m0, n)));
    ls.st(a, o);
}

void convert_matrices(const Ls& ls, uint32_t m, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        __m128 q0 = ls.q(m + 64 * i), q1 = ls.q(m + 64 * i + 16), q2 = ls.q(m + 64 * i + 32), q3 = ls.q(m + 64 * i + 48);
        __m128 o0 = _mm_insert_ps(_mm_blend_ps(_mm_blend_ps(q0, q1, 2), q2, 4), q3, (0 << 6) | (3 << 4));
        __m128 o1 = _mm_insert_ps(_mm_blend_ps(_mm_blend_ps(q1, q2, 2), q0, 4), q3, (1 << 6) | (3 << 4));
        __m128 o2 = _mm_insert_ps(_mm_blend_ps(_mm_blend_ps(q2, q0, 2), q1, 4), q3, (2 << 6) | (3 << 4));
        ls.st(m + 48 * i, o0);
        ls.st(m + 48 * i + 16, o1);
        ls.st(m + 48 * i + 32, o2);
    }
}

void normalize_one_exact(const Ls& ls, uint32_t a) {
    __m128 v = ls.q(a);
    alignas(16) float c[4];
    _mm_store_ps(c, v);
    __m128 x = _mm_set_ss(c[0]), y = _mm_set_ss(c[1]), z = _mm_set_ss(c[2]);
    __m128 l = spuf::fma(z, z, spuf::fma(y, y, spuf::fm(x, x)));
    float r = spuf::frsqest(_mm_cvtss_f32(l));
    ls.st(a, spuf::fm(v, _mm_setr_ps(r, r, r, 1.0f)));
}

inline __m256 frsqest8(__m256 l) {
    __m256i li = _mm256_castps_si256(l);
    __m256i absb = _mm256_and_si256(li, _mm256_set1_epi32(0x7FFFFFFF));
    __m256 r = _mm256_div_ps(_mm256_set1_ps(1.0f), _mm256_sqrt_ps(_mm256_castsi256_ps(absb)));
    __m256i ri = _mm256_castps_si256(r);
    __m256i e255 = _mm256_cmpeq_epi32(_mm256_and_si256(ri, _mm256_set1_epi32(0x7F800000)), _mm256_set1_epi32(0x7F800000));
    ri = _mm256_blendv_epi8(ri, _mm256_or_si256(_mm256_and_si256(ri, _mm256_set1_epi32(int(0x80000000))), _mm256_set1_epi32(0x7FFFFFFF)), e255);
    __m256i e0 = _mm256_cmpeq_epi32(_mm256_and_si256(li, _mm256_set1_epi32(0x7F800000)), _mm256_setzero_si256());
    ri = _mm256_blendv_epi8(ri, _mm256_set1_epi32(0x7FFFFFFF), e0);
    return _mm256_castsi256_ps(ri);
}

inline bool small8(__m256 v) {
    __m256i m = _mm256_and_si256(_mm256_castps_si256(v), _mm256_set1_epi32(0x7FFFFFFF));
    return _mm256_testz_si256(_mm256_cmpgt_epi32(m, _mm256_set1_epi32(0x5D7FFFFF)), _mm256_set1_epi32(-1));
}

inline __m256 pz8(__m256 r) { return _mm256_andnot_ps(_mm256_cmp_ps(r, _mm256_setzero_ps(), _CMP_EQ_OQ), r); }

void normalize(const Ls& ls, uint32_t t, uint32_t n) {
    uint32_t cnt = (n + 7) & ~7u;
    for (uint32_t i = 0; i < cnt; i += 8) {
        __m256 v[4];
        for (int k = 0; k < 4; k++) v[k] = ld8(ls.p, t + 16 * (i + k), t + 16 * (i + 4 + k));
        bool ok = true;
        for (int k = 0; k < 4 && ok; k++) ok = small8(v[k]);
        if (!ok) {
            for (int k = 0; k < 8; k++) normalize_one_exact(ls, t + 16 * (i + k));
            continue;
        }
        __m256 t0 = _mm256_unpacklo_ps(v[0], v[1]);
        __m256 t1 = _mm256_unpackhi_ps(v[0], v[1]);
        __m256 t2 = _mm256_unpacklo_ps(v[2], v[3]);
        __m256 t3 = _mm256_unpackhi_ps(v[2], v[3]);
        __m256 x = _mm256_castpd_ps(_mm256_unpacklo_pd(_mm256_castps_pd(t0), _mm256_castps_pd(t2)));
        __m256 y = _mm256_castpd_ps(_mm256_unpackhi_pd(_mm256_castps_pd(t0), _mm256_castps_pd(t2)));
        __m256 z = _mm256_castpd_ps(_mm256_unpacklo_pd(_mm256_castps_pd(t1), _mm256_castps_pd(t3)));
        __m256 l = _mm256_fmadd_ps(z, z, _mm256_fmadd_ps(y, y, _mm256_mul_ps(x, x)));
        __m256 r = frsqest8(l);
        if (!_mm256_testz_si256(_mm256_cmpeq_epi32(_mm256_and_si256(_mm256_castps_si256(r), _mm256_set1_epi32(0x7F800000)), _mm256_set1_epi32(0x7F800000)), _mm256_set1_epi32(-1))) {
            for (int k = 0; k < 8; k++) normalize_one_exact(ls, t + 16 * (i + k));
            continue;
        }
        __m256 one = _mm256_set1_ps(1.0f);
        __m256 r01 = _mm256_unpacklo_ps(r, r);
        __m256 r23 = _mm256_unpackhi_ps(r, r);
        __m256 s0 = _mm256_blend_ps(_mm256_permute_ps(r01, _MM_SHUFFLE(0, 0, 0, 0)), one, 0x88);
        __m256 s1 = _mm256_blend_ps(_mm256_permute_ps(r01, _MM_SHUFFLE(2, 2, 2, 2)), one, 0x88);
        __m256 s2 = _mm256_blend_ps(_mm256_permute_ps(r23, _MM_SHUFFLE(0, 0, 0, 0)), one, 0x88);
        __m256 s3 = _mm256_blend_ps(_mm256_permute_ps(r23, _MM_SHUFFLE(2, 2, 2, 2)), one, 0x88);
        st8(ls.p, t + 16 * i, t + 16 * (i + 4), pz8(_mm256_mul_ps(v[0], s0)));
        st8(ls.p, t + 16 * (i + 1), t + 16 * (i + 5), pz8(_mm256_mul_ps(v[1], s1)));
        st8(ls.p, t + 16 * (i + 2), t + 16 * (i + 6), pz8(_mm256_mul_ps(v[2], s2)));
        st8(ls.p, t + 16 * (i + 3), t + 16 * (i + 7), pz8(_mm256_mul_ps(v[3], s3)));
    }
}

void blend_exact(const Ls& ls, uint32_t matrices, uint32_t wp, uint32_t pp, uint32_t tp) {
    const __m128 inv255 = _mm_castsi128_ps(_mm_set1_epi32(0x3b808080));
    const __m128 one = _mm_set1_ps(1.0f);
    const __m128 xyz = _mm_castsi128_ps(_mm_setr_epi32(-1, -1, -1, 0));
    __m128 r[3];
    for (int k = 0; k < 4; k++) {
        uint32_t wb = ls.p[(wp + 2 * k) & 0x3ffff];
        uint32_t idx = ls.p[(wp + 2 * k + 1) & 0x3ffff];
        __m128 wf = spuf::fm(_mm_set1_ps(spuf::fl(spuf::cuflt(wb, 155))), inv255);
        uint32_t base = matrices + 48 * idx;
        for (int row = 0; row < 3; row++) {
            __m128 m = ls.q(base + 16 * row);
            r[row] = k == 0 ? spuf::fm(wf, m) : spuf::fma(wf, m, r[row]);
        }
    }
    __m128 r0 = _mm_blendv_ps(one, r[0], xyz);
    __m128 r1 = _mm_and_ps(r[1], xyz);
    __m128 r2 = _mm_and_ps(r[2], xyz);
    alignas(16) float w0[4], w1[4], w2[4];
    _mm_store_ps(w0, r[0]);
    _mm_store_ps(w1, r[1]);
    _mm_store_ps(w2, r[2]);
    __m128 tr = _mm_setr_ps(w0[3], w1[3], w2[3], 0.0f);
    __m128 p = ls.q(pp);
    __m128 o = spuf::fma(p, r0, tr);
    o = spuf::fma(yzx(p), r1, o);
    o = spuf::fma(zxy(p), r2, o);
    ls.st(tp, r0);
    ls.st(tp + 16, r1);
    ls.st(tp + 32, r2);
    ls.st(pp, o);
}

void blend_pair(const Ls& ls, const float* mc, uint32_t nmat, uint32_t matrices, uint32_t wp, uint32_t pp, uint32_t tp) {
    const uint8_t* b = ls.p;
    if (((wp & 0x3ffff) + 16) > 0x40000 || ((pp & 0x3ffff) + 32) > 0x40000) {
        blend_exact(ls, matrices, wp, pp, tp);
        blend_exact(ls, matrices, wp + 8, pp + 16, tp + 48);
        return;
    }
    __m128i raw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + (wp & 0x3ffff)));
    __m128i wsel = _mm_shuffle_epi8(raw, _mm_setr_epi8(0, 2, 4, 6, 8, 10, 12, 14, -1, -1, -1, -1, -1, -1, -1, -1));
    __m128i isel = _mm_shuffle_epi8(raw, _mm_setr_epi8(1, 3, 5, 7, 9, 11, 13, 15, -1, -1, -1, -1, -1, -1, -1, -1));
    __m256 p = ld8(b, pp, pp + 16);
    __m128i over = _mm_subs_epu8(isel, _mm_set1_epi8(char(nmat ? nmat - 1 : 0)));
    if (nmat == 0 || !_mm_testz_si128(over, _mm_setr_epi32(-1, -1, 0, 0)) || !small8(p)) {
        blend_exact(ls, matrices, wp, pp, tp);
        blend_exact(ls, matrices, wp + 8, pp + 16, tp + 48);
        return;
    }
    uint64_t iv = uint64_t(_mm_cvtsi128_si64(isel));
    uint32_t ia[4], ib[4];
    for (int k = 0; k < 4; k++) {
        ia[k] = uint32_t((iv >> (8 * k)) & 0xff);
        ib[k] = uint32_t((iv >> (8 * k + 32)) & 0xff);
    }
    __m256 wf = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(wsel)), _mm256_castsi256_ps(_mm256_set1_epi32(0x3b808080)));
    __m256 w0 = _mm256_permutevar8x32_ps(wf, _mm256_setr_epi32(0, 0, 0, 0, 4, 4, 4, 4));
    __m256 w1 = _mm256_permutevar8x32_ps(wf, _mm256_setr_epi32(1, 1, 1, 1, 5, 5, 5, 5));
    __m256 w2 = _mm256_permutevar8x32_ps(wf, _mm256_setr_epi32(2, 2, 2, 2, 6, 6, 6, 6));
    __m256 w3 = _mm256_permutevar8x32_ps(wf, _mm256_setr_epi32(3, 3, 3, 3, 7, 7, 7, 7));
    __m256 r[3];
    for (int row = 0; row < 3; row++) {
        __m256 m0 = _mm256_loadu2_m128(mc + ib[0] * 12 + row * 4, mc + ia[0] * 12 + row * 4);
        __m256 m1 = _mm256_loadu2_m128(mc + ib[1] * 12 + row * 4, mc + ia[1] * 12 + row * 4);
        __m256 m2 = _mm256_loadu2_m128(mc + ib[2] * 12 + row * 4, mc + ia[2] * 12 + row * 4);
        __m256 m3 = _mm256_loadu2_m128(mc + ib[3] * 12 + row * 4, mc + ia[3] * 12 + row * 4);
        __m256 acc = _mm256_mul_ps(w0, m0);
        acc = _mm256_fmadd_ps(w1, m1, acc);
        acc = _mm256_fmadd_ps(w2, m2, acc);
        r[row] = _mm256_fmadd_ps(w3, m3, acc);
    }
    const __m256 xyz = _mm256_castsi256_ps(_mm256_setr_epi32(-1, -1, -1, 0, -1, -1, -1, 0));
    __m256 r0 = _mm256_blendv_ps(_mm256_set1_ps(1.0f), r[0], xyz);
    __m256 r1 = _mm256_and_ps(r[1], xyz);
    __m256 r2 = _mm256_and_ps(r[2], xyz);
    __m256 u0 = _mm256_unpackhi_ps(r[0], r[1]);
    __m256 tr = _mm256_and_ps(_mm256_shuffle_ps(u0, r[2], _MM_SHUFFLE(3, 3, 3, 2)), xyz);
    __m256 o = _mm256_fmadd_ps(p, r0, tr);
    o = _mm256_fmadd_ps(yzx8(p), r1, o);
    o = _mm256_fmadd_ps(zxy8(p), r2, o);
    st8(ls.p, tp, tp + 48, pz8(r0));
    st8(ls.p, tp + 16, tp + 64, pz8(r1));
    st8(ls.p, tp + 32, tp + 80, pz8(r2));
    st8(ls.p, pp, pp + 16, pz8(o));
}

void transform_exact(const Ls& ls, uint32_t tmp, uint32_t a) {
    __m128 r0 = ls.q(tmp), r1 = ls.q(tmp + 16), r2 = ls.q(tmp + 32);
    __m128 n = ls.q(a);
    __m128 o = spuf::fm(n, r0);
    o = spuf::fma(yzx(n), r1, o);
    o = spuf::fma(zxy(n), r2, o);
    ls.st(a, o);
}

void transform_pair(const Ls& ls, uint32_t tmp, uint32_t a) {
    __m256 n = ld8(ls.p, a, a + 16);
    __m256 r0 = ld8(ls.p, tmp, tmp + 48), r1 = ld8(ls.p, tmp + 16, tmp + 64), r2 = ld8(ls.p, tmp + 32, tmp + 80);
    if (!small8(n) || !small8(_mm256_max_ps(_mm256_max_ps(_mm256_andnot_ps(_mm256_set1_ps(-0.0f), r0), _mm256_andnot_ps(_mm256_set1_ps(-0.0f), r1)), _mm256_andnot_ps(_mm256_set1_ps(-0.0f), r2)))) {
        transform_exact(ls, tmp, a);
        transform_exact(ls, tmp + 48, a + 16);
        return;
    }
    __m256 o = _mm256_mul_ps(n, r0);
    o = _mm256_fmadd_ps(yzx8(n), r1, o);
    o = _mm256_fmadd_ps(zxy8(n), r2, o);
    st8(ls.p, a, a + 16, pz8(o));
}

}

uint32_t skin_test_rsq(uint32_t seed, uint32_t n) {
    spuf::CsrGuard guard;
    uint32_t bad = 0;
    uint32_t x = seed;
    for (uint32_t i = 0; i < n; i++) {
        alignas(32) uint32_t in[8], out[8];
        for (int k = 0; k < 8; k++) {
            x = x * 1664525u + 1013904223u;
            in[k] = (x >> 9) | ((60u + (x % 140u)) << 23);
        }
        _mm256_store_ps(reinterpret_cast<float*>(out), frsqest8(_mm256_load_ps(reinterpret_cast<const float*>(in))));
        for (int k = 0; k < 8; k++) {
            uint32_t e = spuf::bits(spuf::frsqest(spuf::fl(in[k])));
            if (e != out[k]) {
                bad++;
            }
        }
    }
    return bad;
}

bool skin_vertexes(uint8_t* lsp, uint32_t ctx, uint32_t matrices, uint32_t num_matrices, uint32_t weights, uint32_t limit) {
    spuf::CsrGuard guard;
    Ls ls{lsp};
    const uint8_t* cfg = lsp + ctx;
    if (cfg[0] & 0x10) return false;
    uint32_t flavor = cfg[6] & 15;
    if (num_matrices == 0 || flavor == 0) return false;
    if (!(cfg[0] & 0x80)) return false;
    if (flavor != 3) return false;
    uint32_t fmt = cfg[7];
    if (fmt < 2) return false;
    uint32_t nv = spuf::ld16(cfg + 8);
    uint32_t total = (nv + 3) & ~3u;
    if (total <= 11) return false;
    if (num_matrices > 256 || ctx < 0x10000 || ctx + 0x200 > 0x40000) return false;
    uint32_t pos = ls.w(ctx + 92);
    uint32_t t100 = ls.w(ctx + 100);
    uint32_t t104 = ls.w(ctx + 104);
    {
        auto okr = [&](uint32_t a, uint64_t len) { return (a & 15) == 0 && a >= 0x10c80 && uint64_t(a) + len <= limit; };
        uint32_t tmp0 = ls.w(ctx + 16 + 4 * (cfg[0] & 15));
        uint32_t w96 = ls.w(ctx + 96);
        uint64_t tl = 16ull * ((nv + 7) & ~7u) + 64;
        if (!okr(matrices, 64ull * num_matrices) || (weights & 7) || weights < 0x10c80 || uint64_t(weights) + 8ull * total + 16 > limit || !okr(pos, tl) || !okr(tmp0, 48ull * total + 64)) return false;
        if ((t100 && !okr(t100, tl)) || (t104 && !okr(t104, tl)) || (w96 && !okr(w96, tl))) return false;
    }
    if (fmt == 2) convert_matrices(ls, matrices, num_matrices);
    uint32_t tables[2];
    uint32_t ntab = 0;
    if (t100) tables[ntab++] = t100;
    if (t104) tables[ntab++] = t104;
    uint32_t batch = uint32_t((uint64_t(total) * 0xAAAAAAABull) >> 33) & ~3u;
    uint32_t temp = ls.w(ctx + 16 + 4 * (cfg[0] & 15));
    thread_local std::vector<float> cache;
    cache.resize(size_t(num_matrices) * 12 + 16);
    uint32_t nfast = num_matrices > 256 ? 256 : num_matrices;
    auto overlap = [](uint32_t a, uint32_t al, uint32_t b, uint32_t bl) { return a < b + bl && b < a + al; };
    uint32_t mlen = 48 * num_matrices;
    if (overlap(matrices, mlen, pos, 16 * total) || overlap(matrices, mlen, temp, 48 * batch) || (ntab > 0 && overlap(matrices, mlen, tables[0], 16 * total)) || (ntab > 1 && overlap(matrices, mlen, tables[1], 16 * total)) || matrices + mlen > 0x40000) nfast = 0;
    for (uint32_t i = 0; i < nfast; i++)
        for (int r = 0; r < 3; r++) {
            __m128 q = ls.q(matrices + 48 * i + 16 * r);
            __m128i m = _mm_and_si128(_mm_castps_si128(q), _mm_set1_epi32(0x7FFFFFFF));
            if (!_mm_testz_si128(_mm_cmpgt_epi32(m, _mm_set1_epi32(0x5D7FFFFF)), _mm_set1_epi32(-1))) nfast = 0;
            _mm_storeu_ps(cache.data() + i * 12 + r * 4, q);
        }
    uint32_t wp = weights, pp = pos;
    uint32_t tp[2] = {ntab > 0 ? tables[0] : 0, ntab > 1 ? tables[1] : 0};
    uint32_t t96p = ls.w(ctx + 96);
    uint32_t remaining = total;
    while (remaining) {
        uint32_t cnt = remaining > batch ? batch : remaining;
        for (uint32_t v = 0; v < cnt; v += 2) blend_pair(ls, cache.data(), nfast, matrices, wp + 8 * v, pp + 16 * v, temp + 48 * v);
        if (t96p) {
            for (uint32_t v = 0; v < cnt; v++) cofactor_one(ls, temp + 48 * v, t96p + 16 * v);
            t96p += 16 * cnt;
        }
        for (uint32_t v = 0; v < cnt; v += 2)
            for (uint32_t t = 0; t < ntab; t++) transform_pair(ls, temp + 48 * v, tp[t] + 16 * v);
        wp += 8 * cnt;
        pp += 16 * cnt;
        tp[0] += 16 * cnt;
        tp[1] += 16 * cnt;
        remaining -= cnt;
    }
    if (ntab == 2) normalize(ls, tables[1], nv);
    if (ntab >= 1) normalize(ls, tables[0], nv);
    if (uint32_t t96 = ls.w(ctx + 96)) normalize(ls, t96, nv);
    uint32_t mn = matrices > weights ? weights : matrices;
    spuf::st32(lsp + ctx + 88, mn);
    return true;
}

}
