#include "edge_softbody.h"
#include "edge_cull_gen.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace edge::sb {

using namespace edge::xo;

bool sb_group(V* R, uint8_t* ls);
int sb_loop(V* R, uint8_t* ls);
void new_call();
bool sb_6ac8(V* R, uint8_t* ls);

inline void sb_clear(uint8_t* ls, V& r3, V r4, V r5, V& r6, V& r7) {
    V bv = bswap(r5);
    r6 = il(0);
    do {
        r6 = ai(r6, 8);
        uint32_t a = pref(r3);
        for (int j = 0; j < 8; j++) _mm_storeu_si128(reinterpret_cast<__m128i*>(ls + ((a + 16 * j) & 0x3fff0)), bv);
        r7 = cgt(r4, r6);
        r3 = ai(r3, 128);
    } while (pref(r7) != 0);
}

void f_05d90(V* R, uint8_t* ls, Ctl& ctl);
void f_06ac8(V* R, uint8_t* ls, Ctl& ctl);
void f_06ac8_gen(V* R, uint8_t* ls, Ctl& ctl);
void f_07040(V* R, uint8_t* ls, Ctl& ctl);
void f_073e0(V* R, uint8_t* ls, Ctl& ctl);
void f_0b5f0(V* R, uint8_t* ls, Ctl& ctl);
void f_0b600(V* R, uint8_t* ls, Ctl& ctl);
void f_0b670(V* R, uint8_t* ls, Ctl& ctl);

#include "edge_softbody_gen.inc"


void f_06ac8(V* R, uint8_t* ls, Ctl& ctl) {
    if (sb_6ac8(R, ls)) return;
    f_06ac8_gen(R, ls, ctl);
}

namespace {

inline __m128 ldq(const uint8_t* ls, uint32_t a) { return spuf::ld4(ls + (a & 0x3fff0)); }
inline void stq(uint8_t* ls, uint32_t a, __m128 v) { spuf::st4(ls + (a & 0x3fff0), v); }
inline __m128 splat(float f) { return _mm_set1_ps(f); }
inline __m128 bc(__m128 v, int i) {
    alignas(16) float t[4];
    _mm_store_ps(t, v);
    return _mm_set1_ps(t[i]);
}
inline __m128i key(__m128 v) {
    __m128i x = _mm_castps_si128(v);
    __m128i mag = _mm_and_si128(x, _mm_set1_epi32(0x7FFFFFFF));
    __m128i zero = _mm_cmpeq_epi32(_mm_and_si128(x, _mm_set1_epi32(0x7F800000)), _mm_setzero_si128());
    mag = _mm_andnot_si128(zero, mag);
    __m128i neg = _mm_srai_epi32(x, 31);
    return _mm_sub_epi32(_mm_xor_si128(mag, neg), neg);
}
inline __m128 clamp1(__m128 v, __m128 one) {
    __m128i gt = _mm_cmpgt_epi32(key(v), key(one));
    return _mm_castsi128_ps(_mm_or_si128(_mm_andnot_si128(gt, _mm_castps_si128(v)), _mm_and_si128(gt, _mm_castps_si128(one))));
}
inline __m128 fabs_(__m128 v) { return _mm_castsi128_ps(_mm_andnot_si128(_mm_set1_epi32(int(0x80000000u)), _mm_castps_si128(v))); }
inline __m128 fneg_(__m128 v) { return _mm_castsi128_ps(_mm_xor_si128(_mm_set1_epi32(int(0x80000000u)), _mm_castps_si128(v))); }
inline __m128 relu_(__m128 v) {
    __m128i x = _mm_castps_si128(v);
    return _mm_castsi128_ps(_mm_and_si128(x, _mm_cmpgt_epi32(x, _mm_setzero_si128())));
}
inline __m128 posmask(__m128 sel, __m128 e) {
    __m128i m = _mm_cmpgt_epi32(_mm_castps_si128(e), _mm_setzero_si128());
    return _mm_castsi128_ps(_mm_and_si128(_mm_castps_si128(sel), m));
}
inline V tov(__m128 v) { return _mm_shuffle_epi32(_mm_castps_si128(v), _MM_SHUFFLE(0, 1, 2, 3)); }

}

bool group_exact(V* R, uint8_t* ls) {
    uint32_t ent = pref(R[103]);
    uint32_t pos = pref(R[77]);
    uint32_t nrm = pref(R[76]);
    uint32_t a0 = pref(R[99]), a1 = pref(R[97]), a2 = pref(R[98]);
    uint32_t K = pref(R[83]);
    uint32_t n1x16 = pref(R[100]);
    uint32_t sa = pref(R[105]), sbb = pref(R[106]);
    __m128 scale = F(_mm_shuffle_epi32(R[93], _MM_SHUFFLE(0, 1, 2, 3)));
    const __m128 eps = _mm_castsi128_ps(_mm_set1_epi32(0x38d1b717));
    const __m128 one = splat(1.0f), half = splat(0.5f), two = splat(2.0f), three = splat(3.0f), zero = _mm_setzero_ps();
    uint32_t off[8];
    V offq[8];
    for (int j = 0; j < 8; j++) {
        V q = lq(ls, ent + 16 * j);
        offq[j] = rotqbyi(q, 12);
        off[j] = pref(offq[j]);
    }
    __m128 P[2][3], N[2][3];
    for (int h = 0; h < 2; h++) {
        alignas(16) float px[4], py[4], pz[4], nx[4], ny[4], nz[4];
        for (int i = 0; i < 4; i++) {
            alignas(16) float t[4], u[4];
            _mm_store_ps(t, ldq(ls, pos + off[4 * h + i]));
            _mm_store_ps(u, ldq(ls, nrm + off[4 * h + i]));
            px[i] = t[0], py[i] = t[1], pz[i] = t[2];
            nx[i] = u[0], ny[i] = u[1], nz[i] = u[2];
        }
        P[h][0] = _mm_load_ps(px), P[h][1] = _mm_load_ps(py), P[h][2] = _mm_load_ps(pz);
        N[h][0] = _mm_load_ps(nx), N[h][1] = _mm_load_ps(ny), N[h][2] = _mm_load_ps(nz);
    }
    __m128 Dacc[2][3] = {{zero, zero, zero}, {zero, zero, zero}};
    __m128 suma[2] = {eps, eps};
    for (uint32_t k = 0; k < K; k++) {
        __m128 C = ldq(ls, a0 + 16 * k), Gq = ldq(ls, a1 + 16 * k), Dq = ldq(ls, a2 + 16 * k);
        __m128 Gs = spuf::fm(scale, Gq);
        bool directional = n1x16 > 16 * k;
        for (int h = 0; h < 2; h++) {
            __m128 d[3];
            for (int c = 0; c < 3; c++) d[c] = spuf::fs(bc(C, c), P[h][c]);
            __m128 L2 = spuf::fma(d[2], d[2], spuf::fma(d[1], d[1], spuf::fma(d[0], d[0], eps)));
            __m128 r = spuf::frsqest(L2);
            __m128 t = spuf::fm(spuf::fm(L2, r), bc(C, 3));
            t = clamp1(t, one);
            __m128 u = spuf::fs(one, t);
            __m128 s = spuf::fm(spuf::fm(u, u), spuf::fnms(two, u, three));
            __m128 s2 = spuf::fm(s, s);
            __m128 w = spuf::fm(s2, bc(Gs, 3));
            __m128 dir[3];
            for (int c = 0; c < 3; c++) dir[c] = directional ? bc(Dq, c) : spuf::fm(d[c], r);
            __m128 B[3];
            for (int c = 0; c < 3; c++) B[c] = spuf::fm(bc(Gs, c), w);
            for (int c = 0; c < 3; c++) stq(ls, sa + 96 * k + 48 * h + 16 * c, dir[c]);
            for (int c = 0; c < 3; c++) stq(ls, sbb + 96 * k + 48 * h + 16 * c, B[c]);
            __m128 f = spuf::fma(N[h][2], dir[2], spuf::fma(N[h][1], dir[1], spuf::fm(N[h][0], dir[0])));
            __m128 face = clamp1(fabs_(spuf::fma(f, half, half)), one);
            __m128 aa = spuf::fm(fabs_(w), face);
            for (int c = 0; c < 3; c++) Dacc[h][c] = spuf::fma(dir[c], aa, Dacc[h][c]);
            suma[h] = spuf::fa(suma[h], aa);
        }
    }
    __m128 newp[2][3], Dn[2][3], pos_s[2][3], neg_s[2][3];
    for (int h = 0; h < 2; h++) {
        __m128 rs = spuf::frest(suma[h]);
        __m128 inv = spuf::fma(spuf::fnms(rs, suma[h], one), rs, rs);
        for (int c = 0; c < 3; c++) newp[h][c] = spuf::fma(Dacc[h][c], inv, P[h][c]);
        __m128 L2 = spuf::fma(Dacc[h][2], Dacc[h][2], spuf::fma(Dacc[h][1], Dacc[h][1], spuf::fma(Dacc[h][0], Dacc[h][0], eps)));
        __m128 r = spuf::frsqest(L2);
        for (int c = 0; c < 3; c++) {
            Dn[h][c] = spuf::fm(Dacc[h][c], r);
            pos_s[h][c] = zero;
            neg_s[h][c] = zero;
        }
    }
    for (uint32_t k = 0; k < K; k++) {
        for (int h = 0; h < 2; h++) {
            __m128 dir[3], B[3];
            for (int c = 0; c < 3; c++) {
                dir[c] = ldq(ls, sa + 96 * k + 48 * h + 16 * c);
                B[c] = ldq(ls, sbb + 96 * k + 48 * h + 16 * c);
            }
            __m128 e = spuf::fma(Dn[h][2], dir[2], spuf::fma(Dn[h][1], dir[1], spuf::fm(Dn[h][0], dir[0])));
            __m128 ep = posmask(clamp1(e, one), e);
            for (int c = 0; c < 3; c++) {
                __m128 v = spuf::fm(B[c], ep);
                pos_s[h][c] = spuf::fa(pos_s[h][c], relu_(v));
                neg_s[h][c] = spuf::fa(neg_s[h][c], relu_(fneg_(v)));
            }
        }
    }
    R[60] = tov(newp[0][0]);
    R[57] = tov(newp[0][1]);
    R[51] = tov(newp[0][2]);
    R[49] = tov(newp[1][0]);
    R[48] = tov(newp[1][1]);
    R[47] = tov(newp[1][2]);
    R[39] = tov(neg_s[0][0]);
    R[38] = tov(neg_s[0][1]);
    R[36] = tov(neg_s[0][2]);
    R[41] = tov(neg_s[1][0]);
    R[40] = tov(neg_s[1][1]);
    R[37] = tov(neg_s[1][2]);
    R[46] = tov(pos_s[0][0]);
    R[42] = tov(pos_s[0][1]);
    R[43] = tov(pos_s[0][2]);
    R[35] = tov(pos_s[1][0]);
    R[45] = tov(pos_s[1][1]);
    R[34] = tov(pos_s[1][2]);
    R[88] = offq[0];
    R[87] = offq[1];
    R[92] = offq[2];
    R[91] = offq[3];
    R[86] = offq[4];
    R[79] = offq[5];
    R[90] = offq[6];
    R[89] = offq[7];
    R[103] = ai(R[103], 128);
    return true;
}

namespace {

template <bool On>
struct Chk {
    __m256i mx = _mm256_setzero_si256();
    inline __m256 c(__m256 r) {
        if constexpr (On) mx = _mm256_max_epu32(mx, _mm256_and_si256(_mm256_castps_si256(r), _mm256_set1_epi32(0x7FFFFFFF)));
        return r;
    }
    inline __m256 fm(__m256 a, __m256 b) { return c(_mm256_mul_ps(a, b)); }
    inline __m256 fs(__m256 a, __m256 b) { return c(_mm256_sub_ps(a, b)); }
    inline __m256 fa(__m256 a, __m256 b) { return c(_mm256_add_ps(a, b)); }
    inline __m256 fma(__m256 a, __m256 b, __m256 d) { return c(_mm256_fmadd_ps(a, b, d)); }
    inline __m256 fnms(__m256 a, __m256 b, __m256 d) { return c(_mm256_fnmadd_ps(a, b, d)); }
    bool ok() const {
        __m256i b = _mm256_cmpgt_epi32(mx, _mm256_set1_epi32(0x7F7FFFFE));
        return _mm256_testz_si256(b, b);
    }
};

const __m256i kBs8 = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);

inline __m256 pz8(__m256 r) {
    __m256i x = _mm256_castps_si256(r);
    return _mm256_castsi256_ps(_mm256_andnot_si256(_mm256_cmpeq_epi32(x, _mm256_set1_epi32(int(0x80000000u))), x));
}
inline __m256 abs8(__m256 v) { return _mm256_and_ps(v, _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF))); }
inline __m256 rsq8(__m256 x) { return _mm256_div_ps(_mm256_set1_ps(1.0f), _mm256_sqrt_ps(x)); }
inline __m256 rcp8(__m256 x) { return _mm256_div_ps(_mm256_set1_ps(1.0f), x); }

inline void st_half(uint8_t* ls, uint32_t lo, uint32_t hi, __m256 v, bool fix = true) {
    __m256i b = _mm256_shuffle_epi8(_mm256_castps_si256(fix ? pz8(v) : v), kBs8);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(ls + (lo & 0x3fff0)), _mm256_castsi256_si128(b));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(ls + (hi & 0x3fff0)), _mm256_extracti128_si256(b, 1));
}

inline V tov4(__m128 v) { return _mm_shuffle_epi32(_mm_castps_si128(v), _MM_SHUFFLE(0, 1, 2, 3)); }

inline bool small_mag(const uint8_t* p, uint32_t lim) {
    for (int i = 0; i < 4; i++) {
        uint32_t w = spuf::ld32(p + 4 * i) & 0x7FFFFFFFu;
        if (w >= lim) return false;
    }
    return true;
}


struct Ctr {
    uint64_t v = 0;
    void fetch_add(uint64_t x, std::memory_order) { v += x; }
    uint64_t load() const { return v; }
};

thread_local Ctr g_fast, g_exact;


thread_local uint64_t t_call = 0;

struct KParam {
    float c[4];
    float gs[4];
    float d[4];
};

struct ParamCache {
    uint64_t call = ~0ull;
    uint32_t a0 = 0, a1 = 0, a2 = 0, K = 0, scale = 0, n1x16 = 0;
    bool bounded = false;
    KParam p[64];
};

thread_local ParamCache t_pc;

bool overlaps(uint32_t a, uint32_t al, uint32_t b, uint32_t bl) { return a < b + bl && b < a + al; }

const ParamCache* params(const uint8_t* ls, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t K, uint32_t scale_bits, uint32_t n1x16, uint32_t sa, uint32_t sbb) {
    ParamCache& c = t_pc;
    bool same = c.call == t_call && c.a0 == a0 && c.a1 == a1 && c.a2 == a2 && c.K == K && c.scale == scale_bits && c.n1x16 == n1x16;
    if (same) return &c;
    if (overlaps(sa, 96 * K, a0, 16 * K) || overlaps(sa, 96 * K, a1, 16 * K) || overlaps(sa, 96 * K, a2, 16 * K) || overlaps(sbb, 96 * K, a0, 16 * K) || overlaps(sbb, 96 * K, a1, 16 * K) || overlaps(sbb, 96 * K, a2, 16 * K)) return nullptr;
    constexpr uint32_t kLim = 0x4d800000u;
    float scale = spuf::fl(scale_bits);
    c.bounded = (scale_bits & 0x7FFFFFFFu) < kLim;
    for (uint32_t k = 0; k < K; k++) {
        const uint8_t* pc = ls + ((a0 + 16 * k) & 0x3fff0);
        const uint8_t* pg = ls + ((a1 + 16 * k) & 0x3fff0);
        const uint8_t* pd = ls + ((a2 + 16 * k) & 0x3fff0);
        if (!small_mag(pc, kLim) || (spuf::ld32(pc + 12) & 0x80000000u) || !small_mag(pg, kLim) || (16 * k < n1x16 && !small_mag(pd, kLim))) c.bounded = false;
        for (int i = 0; i < 4; i++) {
            c.p[k].c[i] = spuf::ldf(pc + 4 * i);
            c.p[k].gs[i] = scale * spuf::ldf(pg + 4 * i);
            c.p[k].d[i] = spuf::ldf(pd + 4 * i);
        }
    }
    c.call = t_call;
    c.a0 = a0, c.a1 = a1, c.a2 = a2, c.K = K, c.scale = scale_bits, c.n1x16 = n1x16;
    return &c;
}


struct GIn {
    uint32_t ent, pos, nrm, a0, a1, a2, K, n1x16, sa, sbb, scale_bits;
};

struct GOut {
    __m256 np[3], ps[3], ns[3];
    uint32_t off[8];
};

inline bool splat_ok(V v) { return _mm_movemask_epi8(_mm_cmpeq_epi32(v, _mm_shuffle_epi32(v, 0xff))) == 0xffff; }

inline GIn gin(const V* R) {
    GIn g;
    g.ent = pref(R[103]);
    g.pos = pref(R[77]);
    g.nrm = pref(R[76]);
    g.a0 = pref(R[99]), g.a1 = pref(R[97]), g.a2 = pref(R[98]);
    g.K = pref(R[83]);
    g.n1x16 = pref(R[100]);
    g.sa = pref(R[105]), g.sbb = pref(R[106]);
    g.scale_bits = uint32_t(_mm_extract_epi32(R[93], 3));
    return g;
}

template <bool CHECK>
bool gcore(const GIn& in, const ParamCache* pcache, uint32_t ent, uint8_t* ls, GOut& o, bool st) {
    const uint32_t K = in.K, n1x16 = in.n1x16, sa = in.sa, sbb = in.sbb;
    if (!CHECK && !pcache->bounded) return false;
    const KParam* kp = pcache->p;
    constexpr uint32_t kLim = 0x4d800000u;
    const __m256 eps = _mm256_castsi256_ps(_mm256_set1_epi32(0x38d1b717));
    const __m256 one = _mm256_set1_ps(1.0f), half = _mm256_set1_ps(0.5f), two = _mm256_set1_ps(2.0f), three = _mm256_set1_ps(3.0f), zero = _mm256_setzero_ps();
    __m256 P0, P1, P2, N0, N1, N2;
    {
        __m256i bad = _mm256_setzero_si256();
        const __m256i lim = _mm256_set1_epi32(int(kLim - 1)), absm = _mm256_set1_epi32(0x7FFFFFFF);
        for (int j = 0; j < 8; j++) o.off[j] = spuf::ld32(ls + ((ent + 16 * j) & 0x3fff0) + 12);
        auto gather = [&](uint32_t base, __m256& X0, __m256& X1, __m256& X2) {
            __m256i r[4];
            for (int j = 0; j < 4; j++) {
                const __m128i* lo = reinterpret_cast<const __m128i*>(ls + ((base + o.off[j]) & 0x3fff0));
                const __m128i* hi = reinterpret_cast<const __m128i*>(ls + ((base + o.off[j + 4]) & 0x3fff0));
                r[j] = _mm256_shuffle_epi8(_mm256_loadu2_m128i(hi, lo), kBs8);
                if (!CHECK) bad = _mm256_or_si256(bad, _mm256_cmpgt_epi32(_mm256_and_si256(r[j], absm), lim));
            }
            __m256 t0 = _mm256_unpacklo_ps(_mm256_castsi256_ps(r[0]), _mm256_castsi256_ps(r[1]));
            __m256 t1 = _mm256_unpackhi_ps(_mm256_castsi256_ps(r[0]), _mm256_castsi256_ps(r[1]));
            __m256 t2 = _mm256_unpacklo_ps(_mm256_castsi256_ps(r[2]), _mm256_castsi256_ps(r[3]));
            __m256 t3 = _mm256_unpackhi_ps(_mm256_castsi256_ps(r[2]), _mm256_castsi256_ps(r[3]));
            X0 = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(1, 0, 1, 0));
            X1 = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(3, 2, 3, 2));
            X2 = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(1, 0, 1, 0));
        };
        gather(in.pos, P0, P1, P2);
        gather(in.nrm, N0, N1, N2);
        if (!CHECK && !_mm256_testz_si256(bad, bad)) return false;
    }
    Chk<CHECK> X;
    X.c(P0), X.c(P1), X.c(P2), X.c(N0), X.c(N1), X.c(N2);
    alignas(32) __m256 dirs[64][3], bs[64][3], dd[64][3], l2[64], rr[64], ww[64];
    for (uint32_t k = 0; k < K; k++) {
        const KParam& p = kp[k];
        __m256 dx = X.fs(_mm256_broadcast_ss(&p.c[0]), P0), dy = X.fs(_mm256_broadcast_ss(&p.c[1]), P1), dz = X.fs(_mm256_broadcast_ss(&p.c[2]), P2);
        __m256 L2 = X.fma(dz, dz, X.fma(dy, dy, X.fma(dx, dx, eps)));
        dd[k][0] = dx, dd[k][1] = dy, dd[k][2] = dz;
        l2[k] = L2;
        rr[k] = X.c(rsq8(L2));
    }
    for (uint32_t k = 0; k < K; k++) {
        const KParam& p = kp[k];
        __m256 Gx = X.c(_mm256_broadcast_ss(&p.gs[0])), Gy = X.c(_mm256_broadcast_ss(&p.gs[1])), Gz = X.c(_mm256_broadcast_ss(&p.gs[2])), Gw = X.c(_mm256_broadcast_ss(&p.gs[3]));
        __m256 r = rr[k];
        __m256 t = _mm256_min_ps(X.fm(X.fm(l2[k], r), _mm256_broadcast_ss(&p.c[3])), one);
        __m256 u = X.fs(one, t);
        __m256 s = X.fm(X.fm(u, u), X.fnms(two, u, three));
        __m256 w = X.fm(X.fm(s, s), Gw);
        ww[k] = w;
        if (n1x16 > 16 * k) {
            dirs[k][0] = X.c(_mm256_broadcast_ss(&p.d[0]));
            dirs[k][1] = X.c(_mm256_broadcast_ss(&p.d[1]));
            dirs[k][2] = X.c(_mm256_broadcast_ss(&p.d[2]));
        } else {
            dirs[k][0] = X.fm(dd[k][0], r);
            dirs[k][1] = X.fm(dd[k][1], r);
            dirs[k][2] = X.fm(dd[k][2], r);
        }
        bs[k][0] = X.fm(Gx, w), bs[k][1] = X.fm(Gy, w), bs[k][2] = X.fm(Gz, w);
    }
    __m256 D0 = zero, D1 = zero, D2 = zero, suma = eps;
    for (uint32_t k = 0; k < K; k++) {
        __m256 dir0 = dirs[k][0], dir1 = dirs[k][1], dir2 = dirs[k][2];
        __m256 f = X.fma(N2, dir2, X.fma(N1, dir1, X.fm(N0, dir0)));
        __m256 face = _mm256_min_ps(abs8(X.fma(f, half, half)), one);
        __m256 aa = X.fm(abs8(ww[k]), face);
        D0 = X.fma(dir0, aa, D0);
        D1 = X.fma(dir1, aa, D1);
        D2 = X.fma(dir2, aa, D2);
        suma = X.fa(suma, aa);
    }
    __m256 rs = X.c(rcp8(suma));
    __m256 inv = X.fma(X.fnms(rs, suma, one), rs, rs);
    __m256 NP0 = X.fma(D0, inv, P0), NP1 = X.fma(D1, inv, P1), NP2 = X.fma(D2, inv, P2);
    __m256 rd = X.c(rsq8(X.fma(D2, D2, X.fma(D1, D1, X.fma(D0, D0, eps)))));
    __m256 Dn0 = X.fm(D0, rd), Dn1 = X.fm(D1, rd), Dn2 = X.fm(D2, rd);
    __m256 ps0 = zero, ps1 = zero, ps2 = zero, ns0 = zero, ns1 = zero, ns2 = zero;
    for (uint32_t k = 0; k < K; k++) {
        __m256 e = X.fma(Dn2, dirs[k][2], X.fma(Dn1, dirs[k][1], X.fm(Dn0, dirs[k][0])));
        __m256 ep = _mm256_min_ps(_mm256_max_ps(e, zero), one);
        __m256 v0 = X.fm(bs[k][0], ep), v1 = X.fm(bs[k][1], ep), v2 = X.fm(bs[k][2], ep);
        ps0 = X.fa(ps0, _mm256_max_ps(v0, zero));
        ps1 = X.fa(ps1, _mm256_max_ps(v1, zero));
        ps2 = X.fa(ps2, _mm256_max_ps(v2, zero));
        ns0 = X.fs(ns0, _mm256_min_ps(v0, zero));
        ns1 = X.fs(ns1, _mm256_min_ps(v1, zero));
        ns2 = X.fs(ns2, _mm256_min_ps(v2, zero));
    }
    if (!X.ok()) return false;
    o.np[0] = pz8(NP0), o.np[1] = pz8(NP1), o.np[2] = pz8(NP2);
    o.ps[0] = pz8(ps0), o.ps[1] = pz8(ps1), o.ps[2] = pz8(ps2);
    o.ns[0] = pz8(ns0), o.ns[1] = pz8(ns1), o.ns[2] = pz8(ns2);
    if (st) {
        for (uint32_t k = 0; k < K; k++) {
            uint32_t ak = sa + 96 * k, bk = sbb + 96 * k;
            bool raw = n1x16 > 16 * k;
            for (int i = 0; i < 3; i++) {
                st_half(ls, ak + 16 * i, ak + 48 + 16 * i, dirs[k][i], !raw);
                st_half(ls, bk + 16 * i, bk + 48 + 16 * i, bs[k][i], true);
            }
        }
    }
    return true;
}

inline __m256 join(V lo, V hi) { return _mm256_set_m128(F(tov4(F(hi))), F(tov4(F(lo)))); }
inline V lo4(__m256 v) { return tov4(_mm256_castps256_ps128(v)); }
inline V hi4(__m256 v) { return tov4(_mm256_extractf128_ps(v, 1)); }

void out_to_R(V* R, const GOut& o) {
    R[60] = lo4(o.np[0]), R[57] = lo4(o.np[1]), R[51] = lo4(o.np[2]);
    R[49] = hi4(o.np[0]), R[48] = hi4(o.np[1]), R[47] = hi4(o.np[2]);
    R[39] = lo4(o.ns[0]), R[38] = lo4(o.ns[1]), R[36] = lo4(o.ns[2]);
    R[41] = hi4(o.ns[0]), R[40] = hi4(o.ns[1]), R[37] = hi4(o.ns[2]);
    R[46] = lo4(o.ps[0]), R[42] = lo4(o.ps[1]), R[43] = lo4(o.ps[2]);
    R[35] = hi4(o.ps[0]), R[45] = hi4(o.ps[1]), R[34] = hi4(o.ps[2]);
    const int oregs[8] = {88, 87, 92, 91, 86, 79, 90, 89};
    for (int j = 0; j < 8; j++) R[oregs[j]] = _mm_set1_epi32(int(o.off[j]));
}

void R_to_out(const V* R, GOut& o) {
    o.np[0] = join(R[60], R[49]), o.np[1] = join(R[57], R[48]), o.np[2] = join(R[51], R[47]);
    o.ns[0] = join(R[39], R[41]), o.ns[1] = join(R[38], R[40]), o.ns[2] = join(R[36], R[37]);
    o.ps[0] = join(R[46], R[35]), o.ps[1] = join(R[42], R[45]), o.ps[2] = join(R[43], R[34]);
    const int oregs[8] = {88, 87, 92, 91, 86, 79, 90, 89};
    for (int j = 0; j < 8; j++) o.off[j] = pref(R[oregs[j]]);
}

inline void st_quads(uint8_t* ls, uint32_t t, const uint32_t* off, __m256 x, __m256 y, __m256 z, __m256 w) {
    __m256 t0 = _mm256_unpacklo_ps(x, y), t1 = _mm256_unpackhi_ps(x, y), t2 = _mm256_unpacklo_ps(z, w), t3 = _mm256_unpackhi_ps(z, w);
    __m256i q[4];
    q[0] = _mm256_shuffle_epi8(_mm256_castps_si256(_mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(1, 0, 1, 0))), kBs8);
    q[1] = _mm256_shuffle_epi8(_mm256_castps_si256(_mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(3, 2, 3, 2))), kBs8);
    q[2] = _mm256_shuffle_epi8(_mm256_castps_si256(_mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(1, 0, 1, 0))), kBs8);
    q[3] = _mm256_shuffle_epi8(_mm256_castps_si256(_mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(3, 2, 3, 2))), kBs8);
    for (int j = 0; j < 4; j++) _mm_storeu_si128(reinterpret_cast<__m128i*>(ls + ((t + off[j]) & 0x3fff0)), _mm256_castsi256_si128(q[j]));
    for (int j = 0; j < 4; j++) _mm_storeu_si128(reinterpret_cast<__m128i*>(ls + ((t + off[4 + j]) & 0x3fff0)), _mm256_extracti128_si256(q[j], 1));
}

void net_store(uint8_t* ls, uint32_t t73, uint32_t t74, uint32_t t75, const GOut& o) {
    __m256 px = _mm256_permute_ps(o.ns[0], _MM_SHUFFLE(3, 3, 3, 3)), py = _mm256_permute_ps(o.ns[1], _MM_SHUFFLE(3, 3, 3, 3));
    __m256 w = _mm256_blend_ps(_mm256_blend_ps(px, py, 0x22), _mm256_setzero_ps(), 0xCC);
    st_quads(ls, t73, o.off, o.ns[0], o.ns[1], o.ns[2], w);
    st_quads(ls, t75, o.off, o.np[0], o.np[1], o.np[2], o.np[2]);
    st_quads(ls, t74, o.off, o.ps[0], o.ps[1], o.ps[2], o.ps[2]);
}

bool eqv(V v, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
    V e = _mm_set_epi32(int(w0), int(w1), int(w2), int(w3));
    return _mm_movemask_epi8(_mm_cmpeq_epi32(v, e)) == 0xffff;
}

bool masks_ok(const V* R) {
    return eqv(R[80], 0, ~0u, ~0u, 0) && eqv(R[96], 0, 0, ~0u, ~0u) && eqv(R[123], ~0u, 0, ~0u, 0) &&
           eqv(R[111], 0x0c0d0e0f, 0x08090a0b, 0x14151617, 0x10111213) && eqv(R[112], 0x08090a0b, 0x0c0d0e0f, 0x1c1d1e1f, 0x18191a1b) &&
           eqv(R[113], 0x04050607, 0x00010203, 0x10111213, 0x14151617) && eqv(R[118], 0x0c0d0e0f, 0x00010203, 0x10111213, 0x1c1d1e1f) &&
           eqv(R[119], 0x04050607, 0x08090a0b, 0x14151617, 0x18191a1b) && eqv(R[120], 0x04050607, 0x18191a1b, 0x0c0d0e0f, 0x80808080) &&
           eqv(R[121], 0x1c1d1e1f, 0x00010203, 0x0c0d0e0f, 0x80808080);
}

struct Rng {
    uint64_t lo, hi;
};

inline bool ov(Rng a, Rng b) { return a.lo < b.hi && b.lo < a.hi; }

thread_local Ctr g_loop, g_loopdecl, g_checked;

bool loop_safe(const V* R, const uint8_t* ls, uint32_t G) {
    GIn in = gin(R);
    uint32_t ent0 = in.ent & 0x3fff0;
    if (in.ent & 15) return false;
    if (uint64_t(ent0) + 128ull * G > 0x40000) return false;
    uint32_t mn = ~0u, mx = 0;
    for (uint32_t i = 0; i < 8 * G; i++) {
        uint32_t off = spuf::ld32(ls + ent0 + 16 * i + 12);
        mn = off < mn ? off : mn;
        mx = off > mx ? off : mx;
    }
    if (mx >= 0x40000) return false;
    auto tab = [&](uint32_t base) -> Rng { return {uint64_t(base & 0x3fff0) + mn, uint64_t(base & 0x3fff0) + mx + 16}; };
    uint32_t t73 = pref(R[73]), t74 = pref(R[74]), t75 = pref(R[75]);
    if (t73 >= 0x40000 || t74 >= 0x40000 || t75 >= 0x40000 || in.pos >= 0x40000 || in.nrm >= 0x40000) return false;
    Rng W[3] = {tab(t73), tab(t74), tab(t75)};
    Rng Rd[2] = {tab(in.pos), tab(in.nrm)};
    for (const Rng& w : W)
        if (w.hi > 0x40000) return false;
    for (const Rng& r : Rd)
        if (r.hi > 0x40000) return false;
    uint32_t K = in.K > 64 ? 64 : in.K;
    Rng E = {ent0, ent0 + 128ull * G};
    Rng S[2] = {{in.sa, in.sa + 96ull * K + 16}, {in.sbb, in.sbb + 96ull * K + 16}};
    Rng P[3] = {{in.a0 & 0x3fff0, (in.a0 & 0x3fff0) + 16ull * K}, {in.a1 & 0x3fff0, (in.a1 & 0x3fff0) + 16ull * K}, {in.a2 & 0x3fff0, (in.a2 & 0x3fff0) + 16ull * K}};
    for (const Rng& w : W) {
        if (ov(w, E)) return false;
        for (const Rng& s : S)
            if (ov(w, s)) return false;
        for (const Rng& p : P)
            if (ov(w, p)) return false;
    }
    for (const Rng& s : S) {
        if (ov(s, E)) return false;
        for (const Rng& r : Rd)
            if (ov(s, r)) return false;
    }
    return true;
}

bool group_one(V* R, uint8_t* ls, const GIn& in, const ParamCache* pc, GOut& o, bool st, uint64_t& nf, uint64_t& nc, uint64_t& ne) {
    uint32_t ent = pref(R[103]);
    if (pc && gcore<false>(in, pc, ent, ls, o, st)) {
        R[103] = ai(R[103], 128);
        nf++;
        return true;
    }
    if (pc && gcore<true>(in, pc, ent, ls, o, st)) {
        R[103] = ai(R[103], 128);
        nc++;
        return true;
    }
    ne++;
    if (!group_exact(R, ls)) return false;
    R_to_out(R, o);
    return true;
}

bool scratch_disjoint(const V* R) {
    uint64_t K = pref(R[83]) > 64 ? 64 : pref(R[83]);
    Rng a = {pref(R[105]), pref(R[105]) + 96 * K + 16}, b = {pref(R[106]), pref(R[106]) + 96 * K + 16};
    return !ov(a, b);
}

const ParamCache* group_params(const V* R, const uint8_t* ls, const GIn& in) {
    if (!splat_ok(R[93]) || in.K > 64 || in.sa + 96 * in.K + 16 > 0x40000 || in.sbb + 96 * in.K + 16 > 0x40000) return nullptr;
    return params(ls, in.a0, in.a1, in.a2, in.K, in.scale_bits, in.n1x16, in.sa, in.sbb);
}

}

void new_call() { t_call++; }

bool sb_group(V* R, uint8_t* ls) {
    if (!scratch_disjoint(R)) return false;
    t_pc.call = ~0ull;
    static const bool exact_only = std::getenv("EDGE_SB_EXACT") != nullptr;
    uint64_t nf = 0, nc = 0, ne = 0;
    GIn in = gin(R);
    const ParamCache* pc = exact_only ? nullptr : group_params(R, ls, in);
    GOut o;
    bool ok = group_one(R, ls, in, pc, o, true, nf, nc, ne);
    g_fast.fetch_add(nf, std::memory_order_relaxed);
    g_checked.fetch_add(nc, std::memory_order_relaxed);
    g_exact.fetch_add(ne, std::memory_order_relaxed);
    if (!ok) return false;
    out_to_R(R, o);
    return true;
}

int sb_loop(V* R, uint8_t* ls) {
    static const bool exact_only = std::getenv("EDGE_SB_EXACT") != nullptr;
    static const bool per_group = std::getenv("EDGE_SB_PERGROUP") != nullptr;
    uint32_t r114 = pref(R[114]), r107 = pref(R[107]);
    uint32_t G = r107 > r114 ? (r107 - r114 + 7) / 8 : 1;
    if (exact_only || per_group || G > 4096 || !splat_ok(R[114]) || !splat_ok(R[107]) || !masks_ok(R) || !scratch_disjoint(R) || !loop_safe(R, ls, G)) {
        g_loopdecl.fetch_add(1, std::memory_order_relaxed);
        return sb_group(R, ls) ? 1 : 0;
    }
    g_loop.fetch_add(1, std::memory_order_relaxed);
    uint32_t t73 = pref(R[73]), t74 = pref(R[74]), t75 = pref(R[75]);
    GIn in = gin(R);
    const ParamCache* pc = group_params(R, ls, in);
    uint64_t nf = 0, nc = 0, ne = 0;
    for (uint32_t g = 0; g < G; g++) {
        GOut o;
        if (!group_one(R, ls, in, pc, o, g + 1 == G, nf, nc, ne)) return 0;
        net_store(ls, t73, t74, t75, o);
    }
    g_fast.fetch_add(nf, std::memory_order_relaxed);
    g_checked.fetch_add(nc, std::memory_order_relaxed);
    g_exact.fetch_add(ne, std::memory_order_relaxed);
    R[114] = _mm_set1_epi32(int(r114 + 8 * G));
    return 2;
}

namespace {

struct XParam {
    float c[4];
    float g[4];
};

struct XCache {
    uint64_t call = ~0ull;
    uint32_t a0 = 0, a1 = 0, K = 0, n3 = 0, scale = 0;
    bool bounded = false;
    XParam p[1024];
};

thread_local XCache t_xc;

const XCache& xparams(const uint8_t* ls, uint32_t a0, uint32_t a1, uint32_t K, uint32_t n3, uint32_t scale_bits) {
    XCache& c = t_xc;
    if (c.call == t_call && c.a0 == a0 && c.a1 == a1 && c.K == K && c.n3 == n3 && c.scale == scale_bits) return c;
    constexpr uint32_t kLim = 0x4d800000u;
    float scale = spuf::fl(scale_bits);
    c.bounded = (scale_bits & 0x7FFFFFFFu) < kLim;
    for (uint32_t i = 0; i < n3; i++) {
        uint32_t j = K + i;
        const uint8_t* pc = ls + ((a0 + 16 * j) & 0x3fff0);
        const uint8_t* pg = ls + ((a1 + 16 * j) & 0x3fff0);
        if (!small_mag(pc, kLim) || (spuf::ld32(pc + 12) & 0x80000000u) || !small_mag(pg, kLim)) c.bounded = false;
        for (int l = 0; l < 4; l++) {
            c.p[i].c[l] = spuf::ldf(pc + 4 * l);
            c.p[i].g[l] = scale * spuf::ldf(pg + 4 * l);
        }
    }
    c.call = t_call;
    c.a0 = a0, c.a1 = a1, c.K = K, c.n3 = n3, c.scale = scale_bits;
    return c;
}

bool extra_fast(const uint8_t* ls, uint32_t pos, const uint32_t* off, const XCache& xc, uint32_t n3, float init, __m256 q[4]) {
    constexpr uint32_t kLim = 0x4d800000u;
    __m256 P0, P1, P2;
    {
        __m256i bad = _mm256_setzero_si256();
        const __m256i lim = _mm256_set1_epi32(int(kLim - 1)), absm = _mm256_set1_epi32(0x7FFFFFFF);
        __m256i r[4];
        for (int j = 0; j < 4; j++) {
            const __m128i* lo = reinterpret_cast<const __m128i*>(ls + ((pos + off[j]) & 0x3fff0));
            const __m128i* hi = reinterpret_cast<const __m128i*>(ls + ((pos + off[j + 4]) & 0x3fff0));
            r[j] = _mm256_shuffle_epi8(_mm256_loadu2_m128i(hi, lo), kBs8);
            bad = _mm256_or_si256(bad, _mm256_cmpgt_epi32(_mm256_and_si256(r[j], absm), lim));
        }
        if (!_mm256_testz_si256(bad, bad)) return false;
        __m256 t0 = _mm256_unpacklo_ps(_mm256_castsi256_ps(r[0]), _mm256_castsi256_ps(r[1]));
        __m256 t1 = _mm256_unpackhi_ps(_mm256_castsi256_ps(r[0]), _mm256_castsi256_ps(r[1]));
        __m256 t2 = _mm256_unpacklo_ps(_mm256_castsi256_ps(r[2]), _mm256_castsi256_ps(r[3]));
        __m256 t3 = _mm256_unpackhi_ps(_mm256_castsi256_ps(r[2]), _mm256_castsi256_ps(r[3]));
        P0 = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(1, 0, 1, 0));
        P1 = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(3, 2, 3, 2));
        P2 = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(1, 0, 1, 0));
    }
    const __m256 one = _mm256_set1_ps(1.0f), half = _mm256_set1_ps(0.5f), two = _mm256_set1_ps(2.0f), three = _mm256_set1_ps(3.0f);
    const __m256 minnorm = _mm256_castsi256_ps(_mm256_set1_epi32(0x00800000));
    __m256 A0v = _mm256_set1_ps(init), A1v = A0v, A2v = A0v, A3v = A0v;
    __m256 tiny = _mm256_setzero_ps();
    for (uint32_t i = 0; i < n3; i++) {
        const XParam& p = xc.p[i];
        __m256 dx = _mm256_sub_ps(_mm256_broadcast_ss(&p.c[0]), P0), dy = _mm256_sub_ps(_mm256_broadcast_ss(&p.c[1]), P1), dz = _mm256_sub_ps(_mm256_broadcast_ss(&p.c[2]), P2);
        __m256 L2 = _mm256_fmadd_ps(dz, dz, _mm256_fmadd_ps(dy, dy, _mm256_mul_ps(dx, dx)));
        tiny = _mm256_or_ps(tiny, _mm256_cmp_ps(L2, minnorm, _CMP_LT_OQ));
        __m256 y = rsq8(L2);
        __m256 e = _mm256_fnmadd_ps(_mm256_mul_ps(y, L2), y, one);
        __m256 y2 = _mm256_fmadd_ps(e, _mm256_mul_ps(y, half), y);
        __m256 dist = _mm256_mul_ps(y2, L2);
        __m256 t = _mm256_min_ps(_mm256_mul_ps(dist, _mm256_broadcast_ss(&p.c[3])), one);
        __m256 u = _mm256_sub_ps(one, t);
        __m256 s = _mm256_mul_ps(_mm256_mul_ps(u, u), _mm256_fnmadd_ps(two, u, three));
        __m256 Gw = _mm256_broadcast_ss(&p.g[3]);
        __m256 w = _mm256_mul_ps(_mm256_mul_ps(s, s), Gw);
        A0v = _mm256_fmadd_ps(w, _mm256_broadcast_ss(&p.g[0]), A0v);
        A1v = _mm256_fmadd_ps(w, _mm256_broadcast_ss(&p.g[1]), A1v);
        A2v = _mm256_fmadd_ps(w, _mm256_broadcast_ss(&p.g[2]), A2v);
        A3v = _mm256_fmadd_ps(w, Gw, A3v);
    }
    if (!_mm256_testz_ps(tiny, tiny)) return false;
    __m256 t0 = _mm256_unpacklo_ps(A0v, A1v), t1 = _mm256_unpackhi_ps(A0v, A1v), t2 = _mm256_unpacklo_ps(A2v, A3v), t3 = _mm256_unpackhi_ps(A2v, A3v);
    q[0] = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(1, 0, 1, 0)), q[1] = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(3, 2, 3, 2));
    q[2] = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(1, 0, 1, 0)), q[3] = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(3, 2, 3, 2));
    return true;
}

}

namespace {

inline void stv(uint8_t* ls, uint32_t a, V v) { sq(ls, a, v); }

void extra_exact(const uint8_t* ls, uint32_t pos, const uint32_t* off, int n, uint32_t a0, uint32_t a1, uint32_t K, uint32_t n3, __m128 scale, float init, __m128 acc[8]) {
    const __m128 one = _mm_set1_ps(1.0f), half = _mm_set1_ps(0.5f), two = _mm_set1_ps(2.0f), three = _mm_set1_ps(3.0f);
    __m128 P[3];
    {
        alignas(16) float px[4], py[4], pz[4];
        for (int i = 0; i < 4; i++) {
            alignas(16) float t[4];
            _mm_store_ps(t, ldq(ls, pos + off[n + i]));
            px[i] = t[0], py[i] = t[1], pz[i] = t[2];
        }
        P[0] = _mm_load_ps(px), P[1] = _mm_load_ps(py), P[2] = _mm_load_ps(pz);
    }
    for (int i = 0; i < 4; i++) acc[n + i] = _mm_set1_ps(init);
    for (uint32_t j = K; j < K + n3; j++) {
        __m128 C = ldq(ls, a0 + 16 * j), Gs = spuf::fm(scale, ldq(ls, a1 + 16 * j));
        __m128 dx = spuf::fs(bc(C, 0), P[0]), dy = spuf::fs(bc(C, 1), P[1]), dz = spuf::fs(bc(C, 2), P[2]);
        __m128 L2 = spuf::fma(dz, dz, spuf::fma(dy, dy, spuf::fm(dx, dx)));
        __m128 y = spuf::frsqest(L2);
        __m128 e = spuf::fnms(spuf::fm(y, L2), y, one);
        __m128 y2 = spuf::fma(e, spuf::fm(y, half), y);
        __m128 dist = spuf::fm(y2, L2);
        __m128 t = clamp1(spuf::fm(dist, bc(C, 3)), one);
        __m128 u = spuf::fs(one, t);
        __m128 s = spuf::fm(spuf::fm(u, u), spuf::fnms(two, u, three));
        __m128 w = spuf::fm(spuf::fm(s, s), bc(Gs, 3));
        alignas(16) float wv[4];
        _mm_store_ps(wv, w);
        for (int i = 0; i < 4; i++) acc[n + i] = spuf::fma(_mm_set1_ps(wv[i]), Gs, acc[n + i]);
    }
}

void group_exact_6ac8(uint8_t* ls, uint32_t table, uint32_t pos, const uint32_t* off, uint32_t a0, uint32_t a1, uint32_t K, uint32_t n3, __m128 scale, float init) {
    __m128 old[8];
    for (int s = 0; s < 8; s++) old[s] = ldq(ls, table + off[s]);
    __m128 acc[8];
    extra_exact(ls, pos, off, 0, a0, a1, K, n3, scale, init, acc);
    extra_exact(ls, pos, off, 4, a0, a1, K, n3, scale, init, acc);
    for (int s = 0; s < 8; s++) stq(ls, table + off[s], spuf::fma(old[s], _mm_setzero_ps(), acc[s]));
}

bool store_fast(uint8_t* ls, uint32_t table, const uint32_t* off, const __m256* q) {
    __m256 res[4];
    const __m256 absm = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF)), maxf = _mm256_castsi256_ps(_mm256_set1_epi32(0x7F7FFFFF));
    __m256 sp = _mm256_setzero_ps();
    for (int j = 0; j < 4; j++) {
        const __m128i* lo = reinterpret_cast<const __m128i*>(ls + ((table + off[j]) & 0x3fff0));
        const __m128i* hi = reinterpret_cast<const __m128i*>(ls + ((table + off[j + 4]) & 0x3fff0));
        __m256 old = _mm256_castsi256_ps(_mm256_shuffle_epi8(_mm256_loadu2_m128i(hi, lo), kBs8));
        res[j] = _mm256_fmadd_ps(old, _mm256_setzero_ps(), q[j]);
        sp = _mm256_or_ps(sp, _mm256_cmp_ps(_mm256_and_ps(res[j], absm), maxf, _CMP_NLT_UQ));
        res[j] = _mm256_andnot_ps(_mm256_cmp_ps(res[j], _mm256_setzero_ps(), _CMP_EQ_OQ), res[j]);
    }
    if (!_mm256_testz_ps(sp, sp)) return false;
    __m256i b[4];
    for (int j = 0; j < 4; j++) b[j] = _mm256_shuffle_epi8(_mm256_castps_si256(res[j]), kBs8);
    for (int j = 0; j < 4; j++) _mm_storeu_si128(reinterpret_cast<__m128i*>(ls + ((table + off[j]) & 0x3fff0)), _mm256_castsi256_si128(b[j]));
    for (int j = 0; j < 4; j++) _mm_storeu_si128(reinterpret_cast<__m128i*>(ls + ((table + off[j + 4]) & 0x3fff0)), _mm256_extracti128_si256(b[j], 1));
    return true;
}

struct XBatch {
    std::vector<uint32_t> off;
    std::vector<__m256> q;
    std::vector<uint8_t> ok;
};

thread_local XBatch t_xb;

}

bool sb_6ac8(V* R, uint8_t* ls) {
    if (pref(R[0]) != 0x8204) return false;
    if (pref(R[12]) != 0) return false;
    uint32_t sp = pref(R[1]);
    stv(ls, sp - 16, R[80]);
    stv(ls, sp - 32, R[81]);
    stv(ls, sp - 48, R[82]);
    stv(ls, sp - 64, R[83]);
    stv(ls, sp - 80, R[84]);
    stv(ls, sp - 96, R[85]);
    stv(ls, sp - 112, R[126]);
    stv(ls, sp - 144, R[1]);
    uint32_t r9 = pref(R[9]), r10 = pref(R[10]);
    uint32_t table = pref(R[3]);
    if (r9 == r10) {
        int32_t n = int32_t(pref(R[5]));
        if (n > 0) {
            uint32_t iters = (uint32_t(n) + 7) / 8;
            V z = _mm_setzero_si128();
            for (uint32_t i = 0; i < iters * 8; i++) sq(ls, table + 16 * i, z);
        }
        return true;
    }
    int32_t count = int32_t(pref(R[7]));
    if (count <= 0) return true;
    uint32_t blk = pref(R[8]);
    uint32_t w0 = spuf::ld32(ls + (blk & 0x3fff0)), w1 = spuf::ld32(ls + (blk & 0x3fff0) + 4);
    uint32_t a0 = blk + w0, a1 = blk + w1;
    uint32_t K = r9, n3 = r10 - r9;
    uint32_t pos = pref(R[4]), ents = pref(R[6]);
    __m128 scale = F(_mm_shuffle_epi32(R[13], _MM_SHUFFLE(0, 1, 2, 3)));
    float init = (pref(R[11]) & 256) ? 0.5f : 0.0f;
    uint32_t groups = (uint32_t(count) + 7) / 8;
    static const bool exact_only = std::getenv("EDGE_SB_EXACT") != nullptr;
    bool fast = !exact_only && n3 <= 1024 && groups <= 4096 && _mm_movemask_epi8(_mm_cmpeq_epi32(R[13], _mm_shuffle_epi32(R[13], 0xff))) == 0xffff;
    XBatch& xb = t_xb;
    if (fast) {
        const XCache& xc = xparams(ls, a0, a1, K, n3, pref(R[13]));
        xb.off.resize(8 * groups);
        xb.q.resize(4 * groups);
        xb.ok.resize(groups);
        uint32_t mn = ~0u, mx = 0;
        for (uint32_t g = 0; g < groups; g++) {
            uint32_t* off = &xb.off[8 * g];
            for (int s = 0; s < 8; s++) {
                uint32_t o = spuf::ld32(ls + ((ents + 128 * g + 16 * s) & 0x3fff0) + 12);
                off[s] = o;
                mn = o < mn ? o : mn;
                mx = o > mx ? o : mx;
            }
            xb.ok[g] = xc.bounded && extra_fast(ls, pos, off, xc, n3, init, &xb.q[4 * g]);
        }
        auto span = [&](uint32_t base) -> Rng { return {uint64_t(base) + mn, uint64_t(base) + mx + 16}; };
        Rng W = span(table), Rp = span(pos);
        Rng E = {ents, uint64_t(ents) + 128ull * groups};
        Rng P0 = {a0 + 16ull * K, a0 + 16ull * (K + n3)}, P1 = {a1 + 16ull * K, a1 + 16ull * (K + n3)};
        bool linear = mx < 0x40000 && W.hi <= 0x40000 && Rp.hi <= 0x40000 && E.hi <= 0x40000 && P0.hi <= 0x40000 && P1.hi <= 0x40000;
        bool aligned = ((table | pos | ents | a0 | a1 | mn) & 15) == 0;
        if (linear && aligned && !ov(W, Rp) && !ov(W, E) && !ov(W, P0) && !ov(W, P1)) {
            for (uint32_t g = 0; g < groups; g++) {
                const uint32_t* off = &xb.off[8 * g];
                if (xb.ok[g] && store_fast(ls, table, off, &xb.q[4 * g])) continue;
                group_exact_6ac8(ls, table, pos, off, a0, a1, K, n3, scale, init);
            }
            return true;
        }
    }
    for (uint32_t g = 0; g < groups; g++) {
        uint32_t off[8];
        for (int s = 0; s < 8; s++) off[s] = spuf::ld32(ls + ((ents + 128 * g + 16 * s) & 0x3fff0) + 12);
        group_exact_6ac8(ls, table, pos, off, a0, a1, K, n3, scale, init);
    }
    return true;
}

}

namespace edge {

void soft_body_stats(uint64_t* fast, uint64_t* checked, uint64_t* exact, uint64_t* loops, uint64_t* loops_declined) {
    *fast = sb::g_fast.load();
    *checked = sb::g_checked.load();
    *exact = sb::g_exact.load();
    *loops = sb::g_loop.load();
    *loops_declined = sb::g_loopdecl.load();
}

bool soft_body(uint8_t* ls, void* gpr) {
    spuf::CsrGuard guard;
    alignas(16) xo::V R[128];
    std::memcpy(R, gpr, sizeof R);
    xo::Ctl ctl;
    sb::new_call();
    sb::f_073e0(R, ls, ctl);
    if (ctl.fail) return false;
    std::memcpy(gpr, R, sizeof R);
    return true;
}

}
