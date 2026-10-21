#include "edge_xform.h"

#include "spu_float.h"

#include <cstring>

namespace edge {

namespace {

constexpr uint32_t kQuadMask = 0x3fff0;

inline __m128 ldq(const uint8_t* ls, uint32_t a) { return spuf::ld4(ls + (a & kQuadMask)); }
inline void stq(uint8_t* ls, uint32_t a, __m128 v) { spuf::st4(ls + (a & kQuadMask), v); }
inline uint32_t ldw(const uint8_t* ls, uint32_t a) { return spuf::ld32(ls + (a & 0x3fffc)); }

template <int K>
inline __m128 rotw(__m128 v) {
    if constexpr (K == 1) return _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 3, 2, 1));
    else if constexpr (K == 2) return _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 0, 3, 2));
    else return _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 1, 0, 3));
}

inline void quad_words(const uint8_t* ls, uint32_t a, uint32_t w[4]) {
    for (int i = 0; i < 4; i++) w[i] = spuf::ld32(ls + (a & kQuadMask) + 4 * i);
}

}

namespace {

inline __m256i special8(__m256 r) {
    __m256 mag = _mm256_and_ps(r, _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF)));
    return _mm256_castps_si256(_mm256_cmp_ps(mag, _mm256_castsi256_ps(_mm256_set1_epi32(0x7F7FFFFF)), _CMP_NLT_UQ));
}

template <class F>
__attribute__((noinline, cold)) __m256 slow8(__m256 a, __m256 b, __m256 c, __m256 r, __m256i lanes, F f) {
    alignas(32) uint32_t av[8], bv[8], cv[8], rv[8], lv[8];
    _mm256_store_si256(reinterpret_cast<__m256i*>(av), _mm256_castps_si256(a));
    _mm256_store_si256(reinterpret_cast<__m256i*>(bv), _mm256_castps_si256(b));
    _mm256_store_si256(reinterpret_cast<__m256i*>(cv), _mm256_castps_si256(c));
    _mm256_store_si256(reinterpret_cast<__m256i*>(rv), _mm256_castps_si256(r));
    _mm256_store_si256(reinterpret_cast<__m256i*>(lv), lanes);
    for (int i = 0; i < 8; i++)
        if (lv[i]) rv[i] = spuf::from_double(f(spuf::to_double(av[i]), spuf::to_double(bv[i]), spuf::to_double(cv[i])));
    return _mm256_castsi256_ps(_mm256_load_si256(reinterpret_cast<const __m256i*>(rv)));
}

template <class F>
inline __m256 fix8(__m256 a, __m256 b, __m256 c, __m256 r, F f) {
    __m256i bad = special8(r);
    if (!_mm256_testz_si256(bad, bad)) r = slow8(a, b, c, r, bad, f);
    return _mm256_andnot_ps(_mm256_cmp_ps(r, _mm256_setzero_ps(), _CMP_EQ_OQ), r);
}

inline __m256 fa8(__m256 a, __m256 b) { return fix8(a, b, a, _mm256_add_ps(a, b), spuf::AddD{}); }
inline __m256 fm8(__m256 a, __m256 b) { return fix8(a, b, a, _mm256_mul_ps(a, b), spuf::MulD{}); }
inline __m256 fma8(__m256 a, __m256 b, __m256 c) { return fix8(a, b, c, _mm256_fmadd_ps(a, b, c), spuf::MaD{}); }
inline __m256 fnms8(__m256 a, __m256 b, __m256 c) { return fix8(a, b, c, _mm256_fnmadd_ps(a, b, c), spuf::NmsD{}); }

inline __m256 frsqest8(__m256 x) {
    __m256i xi = _mm256_castps_si256(x);
    __m256 ax = _mm256_castsi256_ps(_mm256_and_si256(xi, _mm256_set1_epi32(0x7FFFFFFF)));
    __m256i r = _mm256_castps_si256(_mm256_div_ps(_mm256_set1_ps(1.0f), _mm256_sqrt_ps(ax)));
    __m256i e255 = _mm256_cmpeq_epi32(_mm256_and_si256(r, _mm256_set1_epi32(0x7F800000)), _mm256_set1_epi32(0x7F800000));
    r = _mm256_blendv_epi8(r, _mm256_or_si256(_mm256_and_si256(r, _mm256_set1_epi32(int(0x80000000u))), _mm256_set1_epi32(0x7FFFFFFF)), e255);
    __m256i z = _mm256_cmpeq_epi32(_mm256_and_si256(xi, _mm256_set1_epi32(0x7F800000)), _mm256_setzero_si256());
    r = _mm256_blendv_epi8(r, _mm256_set1_epi32(0x7FFFFFFF), z);
    return _mm256_castsi256_ps(r);
}

inline __m256i key8(__m256 v) {
    __m256i vi = _mm256_castps_si256(v);
    __m256i mag = _mm256_and_si256(vi, _mm256_set1_epi32(0x7FFFFFFF));
    __m256i zero = _mm256_cmpeq_epi32(_mm256_and_si256(vi, _mm256_set1_epi32(0x7F800000)), _mm256_setzero_si256());
    mag = _mm256_andnot_si256(zero, mag);
    __m256i neg = _mm256_srai_epi32(vi, 31);
    return _mm256_sub_epi32(_mm256_xor_si256(mag, neg), neg);
}

inline __m256 ld2(const uint8_t* ls, uint32_t a, uint32_t b) { return _mm256_set_m128(ldq(ls, b), ldq(ls, a)); }
inline void st2(uint8_t* ls, uint32_t a, uint32_t b, __m256 v) {
    stq(ls, a, _mm256_castps256_ps128(v));
    stq(ls, b, _mm256_extractf128_ps(v, 1));
}

const __m256i kBs8 = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
inline __m256 ld8b(const uint8_t* p) { return _mm256_castsi256_ps(_mm256_shuffle_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p)), kBs8)); }
inline void st8b(uint8_t* p, __m256 v) { _mm256_storeu_si256(reinterpret_cast<__m256i*>(p), _mm256_shuffle_epi8(_mm256_castps_si256(v), kBs8)); }

#define SH8(v, i) _mm256_shuffle_ps(v, v, i)

struct Acc {
    __m256 m = _mm256_setzero_ps();
    inline __m256 add(__m256 r) {
        __m256 mag = _mm256_and_ps(r, _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF)));
        m = _mm256_or_ps(m, _mm256_cmp_ps(mag, _mm256_castsi256_ps(_mm256_set1_epi32(0x7F7FFFFF)), _CMP_NLT_UQ));
        return r;
    }
    inline __m256 fa(__m256 a, __m256 b) { return add(_mm256_add_ps(a, b)); }
    inline __m256 fm(__m256 a, __m256 b) { return add(_mm256_mul_ps(a, b)); }
    inline __m256 fma(__m256 a, __m256 b, __m256 c) { return add(_mm256_fmadd_ps(a, b, c)); }
    inline __m256 fnms(__m256 a, __m256 b, __m256 c) { return add(_mm256_fnmadd_ps(a, b, c)); }
    inline bool clean() const { return _mm256_testz_ps(m, m) != 0; }
};

inline __m256 pz8(__m256 r) { return _mm256_andnot_ps(_mm256_cmp_ps(r, _mm256_setzero_ps(), _CMP_EQ_OQ), r); }

inline __m256 xform_exact(__m256 v, __m256 A, __m256 B, __m256 C, __m256 D) {
    __m256 t = fm8(v, A);
    t = fma8(SH8(v, _MM_SHUFFLE(0, 3, 2, 1)), B, t);
    t = fma8(SH8(v, _MM_SHUFFLE(1, 0, 3, 2)), C, t);
    return fma8(SH8(v, _MM_SHUFFLE(2, 1, 0, 3)), D, t);
}

inline __m256 xform(__m256 v, __m256 A, __m256 B, __m256 C, __m256 D) {
    Acc k;
    __m256 t = k.fm(v, A);
    t = k.fma(SH8(v, _MM_SHUFFLE(0, 3, 2, 1)), B, t);
    t = k.fma(SH8(v, _MM_SHUFFLE(1, 0, 3, 2)), C, t);
    t = k.fma(SH8(v, _MM_SHUFFLE(2, 1, 0, 3)), D, t);
    if (k.clean()) [[likely]] return pz8(t);
    return xform_exact(v, A, B, C, D);
}

inline __m256 special_ps(__m256 r) { return _mm256_cmp_ps(_mm256_and_ps(r, _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF))), _mm256_castsi256_ps(_mm256_set1_epi32(0x7F7FFFFF)), _CMP_NLT_UQ); }

__attribute__((noinline)) __m256 binormal_exact(__m256 n, __m256 t) {
    const __m256 mask = _mm256_castsi256_ps(_mm256_setr_epi32(-1, -1, -1, 0, -1, -1, -1, 0));
    const __m256 one = _mm256_set1_ps(1.0f), half = _mm256_set1_ps(0.5f);
    const __m256i eps = _mm256_set1_epi32(spuf::key(0x0da2425f));
    __m256 ts = SH8(t, _MM_SHUFFLE(0, 0, 2, 1));
    __m256 ns = SH8(n, _MM_SHUFFLE(0, 0, 2, 1));
    __m256 c = fnms8(t, ns, fm8(n, ts));
    c = _mm256_and_ps(SH8(c, _MM_SHUFFLE(0, 0, 2, 1)), mask);
    __m256 b = fm8(SH8(n, _MM_SHUFFLE(3, 3, 3, 3)), c);
    __m256 s = fm8(b, b);
    __m256 l = fa8(fa8(s, SH8(s, _MM_SHUFFLE(0, 3, 2, 1))), SH8(s, _MM_SHUFFLE(1, 0, 3, 2)));
    l = SH8(l, 0);
    __m256i gt = _mm256_cmpgt_epi32(key8(l), eps);
    __m256 y = frsqest8(l);
    __m256 e = fnms8(l, fm8(y, y), one);
    __m256 r = fma8(e, fm8(y, half), y);
    __m256 nb = fm8(b, r);
    return _mm256_and_ps(_mm256_blendv_ps(b, nb, _mm256_castsi256_ps(gt)), mask);
}

void binormals_packed(uint8_t* ls, uint32_t t1, uint32_t t2, uint32_t out, int32_t count) {
    const __m256 mask = _mm256_castsi256_ps(_mm256_setr_epi32(-1, -1, -1, 0, -1, -1, -1, 0));
    const __m256 one = _mm256_set1_ps(1.0f), half = _mm256_set1_ps(0.5f);
    const __m256i eps = _mm256_set1_epi32(spuf::key(0x0da2425f));
    for (int32_t base = 0; base < count; base += 8) {
        __m256 nv[4], tv[4], bv[4], lv[4];
        __m256 kb = _mm256_setzero_ps();
        int kbad = 0;
        for (int i = 0; i < 4; i++) {
            uint32_t off = uint32_t(base + 2 * i) * 16;
            __m256 n = ld2(ls, t1 + off, t1 + off + 16);
            __m256 t = ld2(ls, t2 + off, t2 + off + 16);
            __m256 ts = SH8(t, _MM_SHUFFLE(0, 0, 2, 1));
            __m256 ns = SH8(n, _MM_SHUFFLE(0, 0, 2, 1));
            Acc k;
            __m256 c = k.fnms(t, ns, k.fm(n, ts));
            c = _mm256_and_ps(SH8(c, _MM_SHUFFLE(0, 0, 2, 1)), mask);
            __m256 b = k.fm(SH8(n, _MM_SHUFFLE(3, 3, 3, 3)), c);
            __m256 s = k.fm(b, b);
            lv[i] = k.fa(k.fa(s, SH8(s, _MM_SHUFFLE(0, 3, 2, 1))), SH8(s, _MM_SHUFFLE(1, 0, 3, 2)));
            kbad |= (k.clean() ? 0 : 1) << i;
            nv[i] = n;
            tv[i] = t;
            bv[i] = b;
        }
        __m256 u01 = _mm256_unpacklo_ps(lv[0], lv[1]), u23 = _mm256_unpacklo_ps(lv[2], lv[3]);
        __m256 L = _mm256_castpd_ps(_mm256_unpacklo_pd(_mm256_castps_pd(u01), _mm256_castps_pd(u23)));
        __m256 gt = _mm256_castsi256_ps(_mm256_cmpgt_epi32(key8(L), eps));
        Acc kn;
        __m256 y = frsqest8(L);
        __m256 e = kn.fnms(L, kn.fm(y, y), one);
        __m256 r = kn.fma(e, kn.fm(y, half), y);
        kb = kn.m;
        __m256 res[4];
        auto lane = [](__m256 v, int i) {
            switch (i) {
            case 0: return SH8(v, 0x00);
            case 1: return SH8(v, 0x55);
            case 2: return SH8(v, 0xaa);
            default: return SH8(v, 0xff);
            }
        };
        for (int i = 0; i < 4; i++) {
            __m256 gi = lane(gt, i);
            __m256 ri = lane(r, i);
            __m256 nb = _mm256_mul_ps(bv[i], ri);
            __m256 bad = _mm256_or_ps(lane(kb, i), special_ps(nb));
            if (((kbad >> i) & 1) || !_mm256_testz_ps(bad, gi)) {
                res[i] = binormal_exact(nv[i], tv[i]);
                continue;
            }
            res[i] = _mm256_and_ps(_mm256_blendv_ps(pz8(bv[i]), pz8(nb), gi), mask);
        }
        for (int i = 0; i < 4; i++) {
            uint32_t off = out + uint32_t(base + 2 * i) * 16;
            st2(ls, off, off + 16, res[i]);
        }
    }
}

}

void transform_vertexes(uint8_t* ls, uint32_t count, uint32_t src, uint32_t dst, uint32_t matrix, uint32_t r3_out[4]) {
    if (!count) return;
    spuf::CsrGuard guard;
    uint32_t m[4][4];
    for (int r = 0; r < 4; r++) quad_words(ls, matrix + 16 * r, m[r]);
    auto mk = [&](int k) {
        __m128 q = _mm_castsi128_ps(_mm_setr_epi32(int(m[0][k & 3]), int(m[1][(1 + k) & 3]), int(m[2][(2 + k) & 3]), int(m[3][(3 + k) & 3])));
        return _mm256_set_m128(q, q);
    };
    __m256 A = mk(0), B = mk(1), C = mk(2), D = mk(3);
    uint32_t groups = (count + 7) >> 3;
    if (spuf::avx2_on()) {
        const __m256i lim = _mm256_set1_epi32(0x5D7FFFFF);
        const __m256i am = _mm256_set1_epi32(0x7FFFFFFF);
        auto big = [&](__m256 v) { return _mm256_cmpgt_epi32(_mm256_and_si256(_mm256_castps_si256(v), am), lim); };
        __m256i mb = _mm256_or_si256(_mm256_or_si256(big(A), big(B)), _mm256_or_si256(big(C), big(D)));
        bool msmall = _mm256_testz_si256(mb, mb) != 0;
        uint64_t sspan = 128ull * groups + 128, dspan = 128ull * (groups ? groups : 1);
        bool flat = msmall && (src & 15) == 0 && (dst & 15) == 0 && src + sspan <= 0x40000 && dst + dspan <= 0x40000 && (src == dst || uint64_t(dst) + dspan <= src || uint64_t(src) + sspan <= dst);
        if (flat) {
            for (uint32_t g = 0; g < groups; g++) {
                const uint8_t* sp = ls + src + 128 * g;
                __m256 v[4];
                __m256i vb = _mm256_setzero_si256();
                for (int i = 0; i < 4; i++) {
                    v[i] = ld8b(sp + 32 * i);
                    vb = _mm256_or_si256(vb, big(v[i]));
                }
                __m256 o[4];
                if (_mm256_testz_si256(vb, vb)) {
                    for (int i = 0; i < 4; i++) {
                        __m256 t = _mm256_mul_ps(v[i], A);
                        t = _mm256_fmadd_ps(SH8(v[i], _MM_SHUFFLE(0, 3, 2, 1)), B, t);
                        t = _mm256_fmadd_ps(SH8(v[i], _MM_SHUFFLE(1, 0, 3, 2)), C, t);
                        o[i] = pz8(_mm256_fmadd_ps(SH8(v[i], _MM_SHUFFLE(2, 1, 0, 3)), D, t));
                    }
                } else
                    for (int i = 0; i < 4; i++) o[i] = xform(v[i], A, B, C, D);
                uint8_t* dp = ls + dst + 128 * g;
                for (int i = 0; i < 4; i++) st8b(dp + 32 * i, o[i]);
            }
            uint32_t w[4];
            quad_words(ls, src + 128 * groups + 112, w);
            for (int i = 0; i < 4; i++) r3_out[i] = w[(i + 1) & 3];
            return;
        }
    }
    __m256 prev[4];
    for (int i = 0; i < 4; i++) prev[i] = _mm256_setzero_ps();
    for (uint32_t g = 0; g <= groups; g++) {
        __m256 cur[4];
        if (g < groups) {
            for (int i = 0; i < 4; i++) {
                uint32_t va = src + 128 * g + 32 * i;
                cur[i] = xform(ld2(ls, va, va + 16), A, B, C, D);
            }
        } else {
            uint32_t w[4];
            quad_words(ls, src + 128 * g + 112, w);
            for (int i = 0; i < 4; i++) r3_out[i] = w[(i + 1) & 3];
        }
        uint32_t at = dst + 128 * (g ? g - 1 : 0);
        for (int i = 0; i < 4; i++) st2(ls, at + 32 * i, at + 32 * i + 16, prev[i]);
        if (g < groups)
            for (int i = 0; i < 4; i++) prev[i] = cur[i];
    }
}

void compute_binormals(uint8_t* ls, uint32_t ctx, int32_t count, uint32_t r3_out[4]) {
    uint32_t t1 = ldw(ls, ctx + 100), t2 = ldw(ls, ctx + 104);
    uint32_t q[4];
    quad_words(ls, ctx + 96, q);
    if (!t1 || !t2) {
        for (int i = 0; i < 4; i++) r3_out[i] = q[i];
        return;
    }
    if (!q[0]) {
        uint32_t k = ls[ctx & 0x3ffff] & 15;
        uint32_t a = ctx + 16 + 4 * k;
        uint32_t base = a & kQuadMask, rot = a & 15;
        uint8_t b[16];
        for (int i = 0; i < 16; i++) b[i] = ls[base + ((rot + i) & 15)];
        for (int i = 0; i < 4; i++) q[i] = spuf::ld32(b + 4 * i);
        if (!q[0]) {
            for (int i = 0; i < 4; i++) r3_out[i] = 0;
            return;
        }
    }
    for (int i = 0; i < 4; i++) r3_out[i] = q[i];
    if (count <= 0) return;
    spuf::CsrGuard guard;
    uint32_t out = q[0];
    const __m256 mask = _mm256_castsi256_ps(_mm256_setr_epi32(-1, -1, -1, 0, -1, -1, -1, 0));
    const __m256 one = _mm256_set1_ps(1.0f), half = _mm256_set1_ps(0.5f);
    const __m256i eps = _mm256_set1_epi32(spuf::key(0x0da2425f));
    if (spuf::avx2_on()) {
        binormals_packed(ls, t1, t2, out, count);
        return;
    }
    for (int32_t base = 0; base < count; base += 8) {
        __m256 res[4];
        for (int i = 0; i < 4; i++) {
            uint32_t off = uint32_t(base + 2 * i) * 16;
            __m256 n = ld2(ls, t1 + off, t1 + off + 16);
            __m256 t = ld2(ls, t2 + off, t2 + off + 16);
            __m256 ts = SH8(t, _MM_SHUFFLE(0, 0, 2, 1));
            __m256 ns = SH8(n, _MM_SHUFFLE(0, 0, 2, 1));
            Acc k, kn;
            __m256 c = k.fnms(t, ns, k.fm(n, ts));
            c = _mm256_and_ps(SH8(c, _MM_SHUFFLE(0, 0, 2, 1)), mask);
            __m256 b = k.fm(SH8(n, _MM_SHUFFLE(3, 3, 3, 3)), c);
            __m256 s = k.fm(b, b);
            __m256 l = k.fa(k.fa(s, SH8(s, _MM_SHUFFLE(0, 3, 2, 1))), SH8(s, _MM_SHUFFLE(1, 0, 3, 2)));
            l = SH8(l, 0);
            __m256i gt = _mm256_cmpgt_epi32(key8(l), eps);
            __m256 y = frsqest8(l);
            __m256 e = kn.fnms(l, kn.fm(y, y), one);
            __m256 r = kn.fma(e, kn.fm(y, half), y);
            __m256 nb = kn.fm(b, r);
            if (!k.clean() || !_mm256_testz_ps(kn.m, _mm256_castsi256_ps(gt))) {
                c = fnms8(t, ns, fm8(n, ts));
                c = _mm256_and_ps(SH8(c, _MM_SHUFFLE(0, 0, 2, 1)), mask);
                b = fm8(SH8(n, _MM_SHUFFLE(3, 3, 3, 3)), c);
                s = fm8(b, b);
                l = fa8(fa8(s, SH8(s, _MM_SHUFFLE(0, 3, 2, 1))), SH8(s, _MM_SHUFFLE(1, 0, 3, 2)));
                l = SH8(l, 0);
                gt = _mm256_cmpgt_epi32(key8(l), eps);
                y = frsqest8(l);
                e = fnms8(l, fm8(y, y), one);
                r = fma8(e, fm8(y, half), y);
                nb = fm8(b, r);
            } else {
                b = pz8(b);
                nb = pz8(nb);
            }
            res[i] = _mm256_and_ps(_mm256_blendv_ps(b, nb, _mm256_castsi256_ps(gt)), mask);
        }
        for (int i = 0; i < 4; i++) {
            uint32_t off = out + uint32_t(base + 2 * i) * 16;
            st2(ls, off, off + 16, res[i]);
        }
    }
}

bool normalize_vertexes(uint8_t* ls, uint32_t table, const uint32_t count[4], uint32_t r3_out[4]) {
    uint32_t n = count[0];
    if (!n) return false;
    spuf::CsrGuard guard;
    uint32_t groups = ((n + 7) & ~7u) >> 3;
    const __m256 one = _mm256_set1_ps(1.0f);
    for (uint32_t g = 0; g < groups; g++) {
        __m256 v[4], res[4];
        for (int i = 0; i < 4; i++) {
            uint32_t a = table + 128 * g + 32 * i;
            v[i] = ld2(ls, a, a + 16);
        }
        __m256 t0 = _mm256_unpacklo_ps(v[0], v[1]), t1 = _mm256_unpacklo_ps(v[2], v[3]);
        __m256 t2 = _mm256_unpackhi_ps(v[0], v[1]), t3 = _mm256_unpackhi_ps(v[2], v[3]);
        __m256 X = _mm256_shuffle_ps(t0, t1, _MM_SHUFFLE(1, 0, 1, 0));
        __m256 Y = _mm256_shuffle_ps(t0, t1, _MM_SHUFFLE(3, 2, 3, 2));
        __m256 Z = _mm256_shuffle_ps(t2, t3, _MM_SHUFFLE(1, 0, 1, 0));
        Acc k;
        __m256 L = k.fma(Z, Z, k.fma(Y, Y, k.fm(X, X)));
        if (k.clean()) [[likely]] L = pz8(L);
        else L = fma8(Z, Z, fma8(Y, Y, fm8(X, X)));
        __m256 S = frsqest8(L);
        for (int i = 0; i < 4; i++) {
            __m256 sc = _mm256_permutevar8x32_ps(S, _mm256_setr_epi32(i, i, i, i, 4 + i, 4 + i, 4 + i, 4 + i));
            res[i] = fm8(v[i], _mm256_blend_ps(sc, one, 0x88));
        }
        for (int i = 0; i < 4; i++) {
            uint32_t a = table + 128 * g + 32 * i;
            st2(ls, a, a + 16, res[i]);
        }
    }
    for (int i = 0; i < 4; i++) r3_out[i] = uint32_t(0) - (((count[i] + 7) & ~7u) << 4) + 128 * (groups - 1);
    return true;
}

void init_segment(uint8_t* ls, const SegmentInitArgs& a) {
    auto b = [&](uint32_t x) -> uint8_t& { return ls[x & 0x3ffff]; };
    auto qa = [](uint32_t x) { return x & kQuadMask; };
    auto w32 = [&](uint32_t x, uint32_t v) { spuf::st32(ls + (x & 0x3fffc), v); };
    auto cpy16 = [&](uint32_t dst, uint32_t src) { std::memmove(ls + qa(dst), ls + qa(src), 16); };
    const uint32_t G = 0x109a0, T = 0x10a30, M = 0x10b80, H0 = 0x10980, H1 = 0x10990;
    uint32_t ctx = a.r[3] & kQuadMask, cfg = a.r[4], S = a.r[5], O = a.r[6], map = a.r[7];
    uint32_t params = a.r[17];
    uint32_t n = b(qa(S));
    uint32_t count1 = 0;
    uint32_t tcur = T;
    for (uint32_t i = 0; i < n; i++) {
        if (b(map + i) == 0xff) continue;
        uint32_t A = S + 8 + 8 * i;
        if (b(A + 3) == 1 && b(A + 1) == 8) {
            cpy16(H0, params + 80);
            uint32_t fo = b(A + 6);
            for (uint32_t k = 0; k < 8; k++) b(H1 + k) = b(S + 8 + fo + k);
        }
        for (uint32_t k = 0; k < 8; k++) b(tcur + 8 + k) = b(S + 8 * i + 8 + k);
        tcur += 8;
        count1++;
    }
    b(T + 0) = uint8_t(count1);
    b(T + 1) = b(qa(S) + 1);
    b(T + 2) = uint8_t(count1);
    uint32_t n2 = b(qa(O));
    uint32_t count2 = 0, size2 = 0, ucur = G;
    bool keep_all = (a.r[13] & 4) != 0;
    for (uint32_t i = 0; i < n2; i++) {
        uint32_t m = b(map + i);
        if (m == 0xff) continue;
        uint32_t A = O + 8 * i;
        uint32_t id = b(A + 11);
        bool inrange = uint8_t(id - 27) <= 3;
        bool kept = !(inrange && !keep_all);
        for (uint32_t k = 0; k < 8; k++) b(ucur + 8 + k) = b(A + 8 + k);
        b(ucur + 8) = uint8_t(size2);
        b(ucur + 13) = uint8_t(m);
        if (kept) {
            size2 += b(A + 12);
            count2++;
            ucur += 8;
        }
    }
    b(G + 0) = uint8_t(count2);
    b(G + 1) = uint8_t(size2);
    b(G + 2) = uint8_t(count2);
    uint32_t mk = 0;
    for (uint32_t i = 0; i < 16; i++) {
        uint8_t v = b(map + i);
        if (v != 0xff) b(M + mk++) = v;
    }
    uint32_t pq = qa(params + 64);
    b(qa(cfg) + 2) = b(pq + 11);
    b(qa(cfg) + 4) = b(pq + 12);
    uint32_t r5 = a.r[11], r6 = a.r[12], r7 = a.r[9], r8 = a.r[10], r9 = a.r[8], r10 = a.r[16], r11 = a.r[15];
    uint32_t P = qa(cfg);
    uint32_t nverts = spuf::ld16(ls + P + 8);
    uint32_t stride = ((nverts + 7) & ~7u) << 4;
    uint8_t old402 = b(ctx + 402);
    std::memmove(ls + ctx, ls + P, 16);
    uint32_t tables = (b(P) & 15) + 1;
    for (uint32_t k = 0; k < tables; k++) w32(ctx + 16 + 4 * k, r5 + k * stride);
    if (r10)
        for (uint32_t q = 0; q < 8; q++) cpy16(ctx + 208 + 16 * q, r10 + 16 * q);
    if (r11)
        for (uint32_t q = 0; q < 3; q++) cpy16(ctx + 160 + 16 * q, r11 + 16 * q);
    w32(ctx + 80, 0);
    w32(ctx + 84, 0);
    w32(ctx + 88, a.r[14]);
    w32(ctx + 92, 0);
    w32(ctx + 96, 0);
    w32(ctx + 100, 0);
    w32(ctx + 104, 0);
    w32(ctx + 108, T);
    w32(ctx + 112, 0);
    w32(ctx + 116, G);
    w32(ctx + 120, 0);
    w32(ctx + 124, a.r[18]);
    w32(ctx + 128, 0);
    w32(ctx + 132, 0);
    w32(ctx + 136, 0);
    w32(ctx + 140, a.r[19]);
    w32(ctx + 144, 0);
    w32(ctx + 336, r9);
    w32(ctx + 340, r9);
    std::memset(ls + ctx + 352, 0, 16);
    w32(ctx + 368, r5);
    w32(ctx + 372, r5 + r6);
    w32(ctx + 376, r7);
    w32(ctx + 380, r7 + r8);
    w32(ctx + 384, 0);
    w32(ctx + 388, 0);
    w32(ctx + 392, 0);
    b(ctx + 400) = b(P + 10);
    b(ctx + 401) = b(P + 11);
    b(ctx + 402) = uint8_t(old402 & 127);
}

}
