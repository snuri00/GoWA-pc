#include "edge_vdecomp.h"
#include "spu_float.h"

#include <immintrin.h>

#include <cstdlib>
#include <cstring>

namespace edge {

namespace {

constexpr uint32_t kMask = 0x3ffff;
constexpr uint32_t kLs = 0x40000;

struct Sig {
    int id;
    uint32_t size;
    uint64_t head;
    uint64_t hash;
};

const Sig kSigs[] = {
    {10, 0x4e0, 0x8180fc249d088040ull, 0xef1e4ef30bbf9668ull},
    {12, 0x860, 0xd0c0ff2494008040ull, 0x7805b9b2fb660757ull},
    {1, 0x2a0, 0x8180fc24ac008040ull, 0x45ca5dd2448a4602ull},
    {9, 0x4e0, 0x8180fc249d088040ull, 0x7d40061941d6ace1ull},
    {11, 0x6d0, 0x8180fc249c088040ull, 0x207e778488a8d7b7ull},
    {0, 0x2b0, 0x8180fc24aa008040ull, 0xf4b3970956625c75ull},
    {3, 0x610, 0x8180fc2488008040ull, 0x2c6e001d10f9c003ull},
};

uint64_t hash64(const uint8_t* p, uint32_t n) {
    uint64_t h[4] = {0x9e3779b97f4a7c15ull, 0xc2b2ae3d27d4eb4full, 0x165667b19e3779f9ull, 0x27d4eb2f165667c5ull};
    uint32_t n32 = n / 32 * 32;
    for (uint32_t i = 0; i < n32; i += 32)
        for (int k = 0; k < 4; k++) {
            uint64_t w;
            std::memcpy(&w, p + i + 8 * k, 8);
            uint64_t x = (h[k] ^ w) * 0x100000001b3ull;
            h[k] = x ^ (x >> 29);
        }
    for (uint32_t i = n32; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        uint64_t x = (h[0] ^ w) * 0x100000001b3ull;
        h[0] = x ^ (x >> 29);
    }
    uint64_t r = 0;
    for (int k = 0; k < 4; k++) {
        r = (r ^ h[k]) * 0x9e3779b97f4a7c15ull;
        r ^= r >> 31;
    }
    return r;
}

const uint8_t kPatterns[12][16] = {
    {0x80, 0x01, 0x02, 0x03, 0x10, 0x11, 0x12, 0x13, 0x94, 0x15, 0x16, 0x17, 0xbf, 0x80, 0x00, 0x00},
    {0x00, 0x01, 0x02, 0x03, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0xbf, 0x80, 0x00, 0x00},
    {0x80, 0x01, 0x02, 0x03, 0x10, 0x11, 0x12, 0x13, 0x94, 0x15, 0x16, 0x17, 0x3f, 0x80, 0x00, 0x00},
    {0x00, 0x01, 0x02, 0x03, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x3f, 0x80, 0x00, 0x00},
    {0x90, 0x11, 0x12, 0x13, 0x00, 0x01, 0x02, 0x03, 0x94, 0x15, 0x16, 0x17, 0xbf, 0x80, 0x00, 0x00},
    {0x10, 0x11, 0x12, 0x13, 0x00, 0x01, 0x02, 0x03, 0x14, 0x15, 0x16, 0x17, 0xbf, 0x80, 0x00, 0x00},
    {0x90, 0x11, 0x12, 0x13, 0x00, 0x01, 0x02, 0x03, 0x94, 0x15, 0x16, 0x17, 0x3f, 0x80, 0x00, 0x00},
    {0x10, 0x11, 0x12, 0x13, 0x00, 0x01, 0x02, 0x03, 0x14, 0x15, 0x16, 0x17, 0x3f, 0x80, 0x00, 0x00},
    {0x90, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x80, 0x01, 0x02, 0x03, 0xbf, 0x80, 0x00, 0x00},
    {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x00, 0x01, 0x02, 0x03, 0xbf, 0x80, 0x00, 0x00},
    {0x90, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x80, 0x01, 0x02, 0x03, 0x3f, 0x80, 0x00, 0x00},
    {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x00, 0x01, 0x02, 0x03, 0x3f, 0x80, 0x00, 0x00},
};

inline uint32_t rd32(const uint8_t* ls, uint32_t a) { return spuf::ld32(ls + (a & kMask)); }
inline void wr32(uint8_t* ls, uint32_t a, uint32_t v) { spuf::st32(ls + (a & kMask), v); }

inline bool fast() {
    static const bool f = !std::getenv("EDGE_VD_REF");
    return f;
}

bool in_ls(uint32_t lo, uint32_t len) { return lo < kLs && uint64_t(lo) + len <= kLs; }
bool disjoint(uint32_t a, uint32_t al, uint32_t b, uint32_t bl) { return uint64_t(a) + al <= b || uint64_t(b) + bl <= a; }

uint32_t alloc_table(uint8_t* ls, uint32_t ctx, uint32_t attr) {
    uint8_t* flags = ls + ctx + 352;
    uint32_t k = 16;
    for (uint32_t i = 0; i < 16; i++)
        if (flags[i] == 0) {
            k = i;
            break;
        }
    if (k < 16) flags[k] = uint8_t(attr);
    return k;
}

uint64_t window64(const uint8_t* ls, uint32_t a) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | ls[(a + i) & kMask];
    return v;
}

float fixed_to_float(uint32_t field, uint32_t offset, uint32_t frac) {
    uint32_t sum = field + offset;
    int32_t ip = frac >= 32 ? (int32_t(sum) >> 31) : (int32_t(sum) >> frac);
    uint32_t sh = (32 - frac) & 63;
    uint32_t fp = sh >= 32 ? 0 : (sum << sh);
    float a = spuf::fl(spuf::csflt(ip, 155));
    float b = spuf::fl(spuf::cuflt(fp, 123));
    return spuf::fa(a, b);
}

struct PosFmt {
    uint32_t off[3], wid[3], frac[3];
    void load(const uint8_t* ls, uint32_t desc) {
        for (int c = 0; c < 3; c++) {
            off[c] = rd32(ls, desc + 4 * c);
            uint32_t ib = ls[(desc + 0x10 + 2 * c) & kMask];
            frac[c] = ls[(desc + 0x11 + 2 * c) & kMask];
            wid[c] = ib + frac[c];
        }
    }
    bool supported() const { return wid[0] <= 32 && wid[1] <= 32 && wid[2] <= 32 && wid[0] + wid[1] + wid[2] <= 64; }
    void decode(const uint8_t* ls, uint32_t a, uint8_t* o) const {
        uint64_t w = window64(ls, a);
        uint32_t pos = 0;
        for (int c = 0; c < 3; c++) {
            uint32_t width = wid[c] & 63;
            uint32_t field = width == 0 ? 0 : uint32_t((w << pos) >> (64 - width));
            pos += width;
            spuf::stf(o + 4 * c, fixed_to_float(field, off[c], frac[c]));
        }
        spuf::stf(o + 12, 1.0f);
    }
};

inline uint64_t ld64be(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return __builtin_bswap64(v);
}

const __m128i kBswap32 = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);

__m128 cuflt_m32(__m128i fp) {
    __m256d d = _mm256_cvtepi32_pd(fp);
    __m256d neg = _mm256_castsi256_pd(_mm256_cvtepi32_epi64(_mm_srai_epi32(fp, 31)));
    d = _mm256_add_pd(d, _mm256_and_pd(neg, _mm256_set1_pd(4294967296.0)));
    d = _mm256_mul_pd(d, _mm256_set1_pd(1.0 / 4294967296.0));
    return _mm256_cvtpd_ps(d);
}

void decode_pos_fast(const uint8_t* ls, uint32_t verts, uint32_t stride, uint32_t n, const PosFmt& pf, uint8_t* out) {
    __m256i sh_l[3], sh_r[3];
    __m128i off[3], fr[3], frs[3];
    uint32_t pos = 0;
    for (int c = 0; c < 3; c++) {
        sh_l[c] = _mm256_set1_epi64x(pos);
        sh_r[c] = _mm256_set1_epi64x(64 - pf.wid[c]);
        pos += pf.wid[c];
        off[c] = _mm_set1_epi32(int(pf.off[c]));
        fr[c] = _mm_set1_epi32(int(pf.frac[c]));
        frs[c] = _mm_set1_epi32(int((32 - pf.frac[c]) & 63));
    }
    const __m256i pack = _mm256_setr_epi32(0, 2, 4, 6, 1, 3, 5, 7);
    const __m128 one = _mm_set1_ps(1.0f);
    uint32_t v = 0;
    for (; v + 4 <= n; v += 4) {
        const uint8_t* p = ls + verts + stride * v;
        __m256i w = _mm256_setr_epi64x(int64_t(ld64be(p)), int64_t(ld64be(p + stride)), int64_t(ld64be(p + 2 * stride)), int64_t(ld64be(p + 3 * stride)));
        __m128 r[3];
        for (int c = 0; c < 3; c++) {
            __m256i f = _mm256_srlv_epi64(_mm256_sllv_epi64(w, sh_l[c]), sh_r[c]);
            __m128i f32 = _mm256_castsi256_si128(_mm256_permutevar8x32_epi32(f, pack));
            __m128i sum = _mm_add_epi32(f32, off[c]);
            __m128i ip = _mm_srav_epi32(sum, fr[c]);
            __m128i fp = _mm_sllv_epi32(sum, frs[c]);
            __m128 a = _mm_cvtepi32_ps(ip);
            __m128 b = cuflt_m32(fp);
            r[c] = spuf::pos_zero(_mm_add_ps(a, b));
        }
        __m128 x = r[0], y = r[1], z = r[2], o = one;
        _MM_TRANSPOSE4_PS(x, y, z, o);
        uint8_t* dst = out + 16 * v;
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), _mm_shuffle_epi8(_mm_castps_si128(x), kBswap32));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 16), _mm_shuffle_epi8(_mm_castps_si128(y), kBswap32));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 32), _mm_shuffle_epi8(_mm_castps_si128(z), kBswap32));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 48), _mm_shuffle_epi8(_mm_castps_si128(o), kBswap32));
    }
    for (; v < n; v++) pf.decode(ls, verts + stride * v, out + 16 * v);
}

bool run_pos(uint8_t* ls, uint32_t ctx, uint32_t verts, uint32_t nv, const PosFmt& pf, uint32_t stride, Outputs* outs) {
    uint32_t k = alloc_table(ls, ctx, 1);
    uint32_t table = rd32(ls, ctx + 16 + 4 * k);
    wr32(ls, ctx + 92, table);
    uint32_t n2 = (nv + 1) & ~1u;
    bool ok = fast() && in_ls(verts, stride * n2 + 8) && in_ls(table, 16 * n2) && disjoint(verts, stride * n2 + 8, table, 16 * n2);
    if (ok) decode_pos_fast(ls, verts, stride, n2, pf, ls + table);
    else
        for (uint32_t v = 0; v < n2; v++) pf.decode(ls, verts + stride * v, ls + ((table + 16 * v) & kMask));
    if (outs) {
        outs->add(table, table + 16 * n2);
        outs->add(ctx + 92, ctx + 96);
        outs->add(ctx + 352, ctx + 368);
    }
    return true;
}

bool run_copy(uint8_t* ls, uint32_t ctx, uint32_t verts, uint32_t nv, uint32_t stride, Outputs* outs) {
    uint32_t k = alloc_table(ls, ctx, 1);
    uint32_t table = rd32(ls, ctx + 16 + 4 * k);
    wr32(ls, ctx + 92, table);
    uint32_t n2 = (nv + 1) & ~1u;
    bool ok = fast() && in_ls(verts, stride * n2 + 16) && in_ls(table, 16 * n2) && disjoint(verts, stride * n2 + 16, table, 16 * n2);
    if (ok) {
        const __m128i blend = _mm_setr_epi32(0, 0, 0, -1);
        const __m128i one = _mm_setr_epi32(0, 0, 0, 0x0000803f);
        for (uint32_t v = 0; v < n2; v++) {
            __m128i q = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ls + verts + stride * v));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(ls + table + 16 * v), _mm_blendv_epi8(q, one, blend));
        }
    } else {
        for (uint32_t v = 0; v < n2; v++) {
            uint8_t q[16];
            for (int i = 0; i < 12; i++) q[i] = ls[(verts + stride * v + i) & kMask];
            spuf::stf(q + 12, 1.0f);
            for (int i = 0; i < 16; i++) ls[(table + 16 * v + i) & kMask] = q[i];
        }
    }
    if (outs) {
        outs->add(table, table + 16 * n2);
        outs->add(ctx + 92, ctx + 96);
        outs->add(ctx + 352, ctx + 368);
    }
    return true;
}

struct Q {
    uint32_t w[4];
};

Q qload(const uint8_t* p) {
    Q q;
    for (int i = 0; i < 4; i++) q.w[i] = spuf::ld32(p + 4 * i);
    return q;
}

uint8_t qbyte(const Q& q, uint32_t i) { return uint8_t(q.w[i >> 2] >> (24 - 8 * (i & 3))); }

Q shufb2(const Q& a, const Q& b, const uint8_t* ctl) {
    Q r;
    for (int i = 0; i < 4; i++) {
        uint32_t v = 0;
        for (int j = 0; j < 4; j++) {
            uint32_t c = ctl[4 * i + j];
            uint32_t x;
            if (c >= 0xe0) x = 0x80;
            else if (c >= 0xc0) x = 0xff;
            else if (c >= 0x80) x = 0;
            else x = (c & 0x1f) < 16 ? qbyte(a, c & 0xf) : qbyte(b, c & 0xf);
            v = (v << 8) | x;
        }
        r.w[i] = v;
    }
    return r;
}

struct UnitVec {
    float a[4];
    uint32_t root[4];
};

const float kUA = spuf::fl(0x46352e7cu);
const float kUB = spuf::fl(0xc6353150u);

void unit_pair(uint32_t ga, uint32_t gb, UnitVec& u) {
    float x[4] = {spuf::fl(0x3f800000u | ((ga >> 14) & 0x3ff)), spuf::fl(0x3f800000u | ((ga >> 4) & 0x3ff)), spuf::fl(0x3f800000u | ((gb >> 14) & 0x3ff)), spuf::fl(0x3f800000u | ((gb >> 4) & 0x3ff))};
    float sq[4];
    for (int i = 0; i < 4; i++) u.a[i] = spuf::fma(x[i], kUA, kUB);
    for (int i = 0; i < 4; i++) sq[i] = spuf::fm(u.a[i], u.a[i]);
    for (int i = 0; i < 4; i++) {
        float s = spuf::fa(sq[i], i < 3 ? sq[i + 1] : 0.0f);
        float m = spuf::fs(1.0f, s);
        float r = spuf::frsqest(m);
        u.root[i] = spuf::bits(spuf::fm(m, r));
    }
}

void unit_out(const UnitVec& u, const uint8_t* pat, bool second, uint8_t* out) {
    Q p = qload(pat);
    Q s1;
    for (int i = 0; i < 3; i++) s1.w[i] = (u.root[i] & 0x7fffffffu) | (p.w[i] & 0x80000000u);
    s1.w[3] = p.w[3];
    Q s2{{spuf::bits(u.a[0]), spuf::bits(u.a[1]), spuf::bits(u.a[2]), spuf::bits(u.a[3])}};
    uint8_t ctl[16];
    for (int i = 0; i < 3; i++) {
        uint32_t c = (p.w[i] & 0x7fffffffu) + (second ? 0x08080808u : 0u);
        for (int j = 0; j < 4; j++) ctl[4 * i + j] = uint8_t(c >> (24 - 8 * j));
    }
    ctl[12] = 0x0c;
    ctl[13] = 0x0d;
    ctl[14] = 0x0e;
    ctl[15] = 0x0f;
    Q r = shufb2(s1, s2, ctl);
    for (int i = 0; i < 4; i++) spuf::st32(out + 4 * i, r.w[i]);
}

__m128i shufb_be(__m128i a, __m128i b, __m128i ctl) {
    __m128i idx = _mm_and_si128(ctl, _mm_set1_epi8(0x0f));
    __m128i ra = _mm_shuffle_epi8(a, idx);
    __m128i rb = _mm_shuffle_epi8(b, idx);
    __m128i r = _mm_blendv_epi8(ra, rb, _mm_slli_epi16(ctl, 3));
    if (_mm_movemask_epi8(ctl) == 0) return r;
    __m128i t = _mm_and_si128(ctl, _mm_set1_epi8(static_cast<char>(0xE0)));
    __m128i special = _mm_or_si128(_mm_cmpeq_epi8(t, _mm_set1_epi8(static_cast<char>(0xC0))), _mm_and_si128(_mm_cmpeq_epi8(t, _mm_set1_epi8(static_cast<char>(0xE0))), _mm_set1_epi8(static_cast<char>(0x80))));
    return _mm_blendv_epi8(r, special, ctl);
}

struct PatSet {
    __m128i pat[16];
    __m128i ctl[2][16];
    void build(const uint8_t (*pats)[16]) {
        const __m128i clr = _mm_setr_epi8(0x7f, -1, -1, -1, 0x7f, -1, -1, -1, 0x7f, -1, -1, -1, 0, 0, 0, 0);
        const __m128i tail = _mm_setr_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0c, 0x0d, 0x0e, 0x0f);
        for (int s = 0; s < 16; s++) {
            pat[s] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pats[s]));
            __m128i c = _mm_or_si128(_mm_and_si128(pat[s], clr), tail);
            ctl[0][s] = c;
            __m128i le = _mm_shuffle_epi8(c, kBswap32);
            le = _mm_add_epi32(le, _mm_setr_epi32(0x08080808, 0x08080808, 0x08080808, 0));
            ctl[1][s] = _mm_shuffle_epi8(le, kBswap32);
        }
    }
};

inline __m128 frsqest_v(__m128 m) {
    __m128i mi = _mm_and_si128(_mm_castps_si128(m), _mm_set1_epi32(0x7fffffff));
    __m128i zero = _mm_cmpeq_epi32(_mm_and_si128(mi, _mm_set1_epi32(0x7f800000)), _mm_setzero_si128());
    __m128 r = _mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(_mm_castsi128_ps(mi)));
    __m128i ri = _mm_castps_si128(r);
    __m128i e255 = _mm_cmpeq_epi32(_mm_and_si128(ri, _mm_set1_epi32(0x7f800000)), _mm_set1_epi32(0x7f800000));
    ri = _mm_blendv_epi8(ri, _mm_or_si128(_mm_and_si128(ri, _mm_set1_epi32(int(0x80000000u))), _mm_set1_epi32(0x7fffffff)), e255);
    ri = _mm_blendv_epi8(ri, _mm_set1_epi32(0x7fffffff), zero);
    return _mm_castsi128_ps(ri);
}

inline void unit_fast(uint32_t ga, uint32_t gb, const PatSet& ps, uint8_t* oa, uint8_t* ob) {
    __m128i f = _mm_setr_epi32(int((ga >> 14) & 0x3ff), int((ga >> 4) & 0x3ff), int((gb >> 14) & 0x3ff), int((gb >> 4) & 0x3ff));
    __m128 x = _mm_castsi128_ps(_mm_or_si128(f, _mm_set1_epi32(0x3f800000)));
    __m128 a = spuf::fma(x, _mm_set1_ps(kUA), _mm_set1_ps(kUB));
    __m128 sq = spuf::fm(a, a);
    __m128 sh = _mm_castsi128_ps(_mm_srli_si128(_mm_castps_si128(sq), 4));
    __m128 s = spuf::fa(sq, sh);
    __m128 m = spuf::fs(_mm_set1_ps(1.0f), s);
    __m128 root = spuf::fm(m, frsqest_v(m));
    __m128i rbe = _mm_shuffle_epi8(_mm_castps_si128(root), kBswap32);
    __m128i abe = _mm_shuffle_epi8(_mm_castps_si128(a), kBswap32);
    const __m128i sm = _mm_setr_epi8(-128, 0, 0, 0, -128, 0, 0, 0, -128, 0, 0, 0, -1, -1, -1, -1);
    uint32_t sa = ga & 15, sb = gb & 15;
    __m128i s1a = _mm_or_si128(_mm_andnot_si128(sm, rbe), _mm_and_si128(sm, ps.pat[sa]));
    __m128i s1b = _mm_or_si128(_mm_andnot_si128(sm, rbe), _mm_and_si128(sm, ps.pat[sb]));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(oa), shufb_be(s1a, abe, ps.ctl[0][sa]));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(ob), shufb_be(s1b, abe, ps.ctl[1][sb]));
}

template <class S>
inline __m256 fix8(__m256 a, __m256 b, __m256 c, __m256 r, S sl) {
    __m256 mag = _mm256_and_ps(r, _mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff)));
    __m256 bad = _mm256_cmp_ps(mag, _mm256_castsi256_ps(_mm256_set1_epi32(0x7f7fffff)), _CMP_NLT_UQ);
    if (!_mm256_testz_ps(bad, bad)) {
        __m128 lo = spuf::fix(_mm256_castps256_ps128(a), _mm256_castps256_ps128(b), _mm256_castps256_ps128(c), _mm256_castps256_ps128(r), sl);
        __m128 hi = spuf::fix(_mm256_extractf128_ps(a, 1), _mm256_extractf128_ps(b, 1), _mm256_extractf128_ps(c, 1), _mm256_extractf128_ps(r, 1), sl);
        return _mm256_set_m128(hi, lo);
    }
    return _mm256_andnot_ps(_mm256_cmp_ps(r, _mm256_setzero_ps(), _CMP_EQ_OQ), r);
}

inline __m256 frsqest8(__m256 m) {
    __m256i mi = _mm256_and_si256(_mm256_castps_si256(m), _mm256_set1_epi32(0x7fffffff));
    __m256i zero = _mm256_cmpeq_epi32(_mm256_and_si256(mi, _mm256_set1_epi32(0x7f800000)), _mm256_setzero_si256());
    __m256 r = _mm256_div_ps(_mm256_set1_ps(1.0f), _mm256_sqrt_ps(_mm256_castsi256_ps(mi)));
    __m256i ri = _mm256_castps_si256(r);
    __m256i e255 = _mm256_cmpeq_epi32(_mm256_and_si256(ri, _mm256_set1_epi32(0x7f800000)), _mm256_set1_epi32(0x7f800000));
    ri = _mm256_blendv_epi8(ri, _mm256_or_si256(_mm256_and_si256(ri, _mm256_set1_epi32(int(0x80000000u))), _mm256_set1_epi32(0x7fffffff)), e255);
    ri = _mm256_blendv_epi8(ri, _mm256_set1_epi32(0x7fffffff), zero);
    return _mm256_castsi256_ps(ri);
}

inline void unit_fast2(uint32_t ga, uint32_t gb, uint32_t gc, uint32_t gd, const PatSet& ps, uint8_t* out) {
    __m256i f = _mm256_setr_epi32(int((ga >> 14) & 0x3ff), int((ga >> 4) & 0x3ff), int((gb >> 14) & 0x3ff), int((gb >> 4) & 0x3ff), int((gc >> 14) & 0x3ff), int((gc >> 4) & 0x3ff), int((gd >> 14) & 0x3ff), int((gd >> 4) & 0x3ff));
    __m256 x = _mm256_castsi256_ps(_mm256_or_si256(f, _mm256_set1_epi32(0x3f800000)));
    __m256 A = _mm256_set1_ps(kUA), B = _mm256_set1_ps(kUB);
    __m256 a = fix8(x, A, B, _mm256_fmadd_ps(x, A, B), spuf::MaD{});
    __m256 sq = fix8(a, a, a, _mm256_mul_ps(a, a), spuf::MulD{});
    __m256 sh = _mm256_castsi256_ps(_mm256_srli_si256(_mm256_castps_si256(sq), 4));
    __m256 s = fix8(sq, sh, sq, _mm256_add_ps(sq, sh), spuf::AddD{});
    __m256 one = _mm256_set1_ps(1.0f);
    __m256 m = fix8(one, s, one, _mm256_sub_ps(one, s), spuf::SubD{});
    __m256 rq = frsqest8(m);
    __m256 root = fix8(m, rq, m, _mm256_mul_ps(m, rq), spuf::MulD{});
    const __m256i bs = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    __m256i rbe = _mm256_shuffle_epi8(_mm256_castps_si256(root), bs);
    __m256i abe = _mm256_shuffle_epi8(_mm256_castps_si256(a), bs);
    const __m128i sm = _mm_setr_epi8(-128, 0, 0, 0, -128, 0, 0, 0, -128, 0, 0, 0, -1, -1, -1, -1);
    __m128i r0 = _mm256_castsi256_si128(rbe), r1 = _mm256_extracti128_si256(rbe, 1);
    __m128i a0 = _mm256_castsi256_si128(abe), a1 = _mm256_extracti128_si256(abe, 1);
    uint32_t s0 = ga & 15, s1 = gb & 15, s2 = gc & 15, s3 = gd & 15;
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out), shufb_be(_mm_or_si128(_mm_andnot_si128(sm, r0), _mm_and_si128(sm, ps.pat[s0])), a0, ps.ctl[0][s0]));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 16), shufb_be(_mm_or_si128(_mm_andnot_si128(sm, r0), _mm_and_si128(sm, ps.pat[s1])), a0, ps.ctl[1][s1]));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 32), shufb_be(_mm_or_si128(_mm_andnot_si128(sm, r1), _mm_and_si128(sm, ps.pat[s2])), a1, ps.ctl[0][s2]));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 48), shufb_be(_mm_or_si128(_mm_andnot_si128(sm, r1), _mm_and_si128(sm, ps.pat[s3])), a1, ps.ctl[1][s3]));
}

bool run_unit(uint8_t* ls, uint32_t ctx, uint32_t verts, uint32_t nv, const PosFmt& pf, const uint8_t* spill, uint32_t stride, int ngroups, const uint32_t* attrs, const uint32_t* slots, Outputs* outs, bool raw_pos = false, uint32_t uoff = 6) {
    auto decode_one = [&](uint32_t a, uint8_t* o) {
        if (raw_pos) {
            for (int i = 0; i < 12; i++) o[i] = ls[(a + i) & kMask];
            spuf::stf(o + 12, 1.0f);
        } else
            pf.decode(ls, a, o);
    };
    uint32_t k1 = alloc_table(ls, ctx, 1);
    uint32_t t0 = rd32(ls, ctx + 16 + 4 * k1);
    uint32_t tg[2] = {};
    for (int g = 0; g < ngroups; g++) {
        uint32_t k = alloc_table(ls, ctx, attrs[g]);
        tg[g] = rd32(ls, ctx + 16 + 4 * k);
    }
    wr32(ls, ctx + 92, t0);
    for (int g = 0; g < ngroups; g++) wr32(ls, ctx + slots[g], tg[g]);
    uint8_t pats[16][16];
    std::memcpy(pats, kPatterns, sizeof kPatterns);
    std::memcpy(pats[12], spill, 64);
    uint32_t n2 = (nv + 1) & ~1u;
    uint32_t pairs = n2 / 2;
    bool ok = fast() && in_ls(verts, stride * n2 + 16) && in_ls(t0, 16 * n2) && disjoint(verts, stride * n2 + 16, t0, 16 * n2);
    for (int g = 0; g < ngroups && ok; g++) {
        ok = in_ls(tg[g], 16 * n2 + 32) && disjoint(verts, stride * n2 + 16, tg[g], 16 * n2 + 32) && disjoint(t0, 16 * n2, tg[g], 16 * n2 + 32);
        for (int h = 0; h < g && ok; h++) ok = disjoint(tg[h], 16 * n2, tg[g], 16 * n2);
    }
    if (ok) {
        PatSet ps;
        ps.build(pats);
        if (raw_pos) {
            for (uint32_t v = 0; v < n2; v++) {
                std::memcpy(ls + t0 + 16 * v, ls + verts + stride * v, 12);
                spuf::stf(ls + t0 + 16 * v + 12, 1.0f);
            }
        } else
            decode_pos_fast(ls, verts, stride, n2, pf, ls + t0);
        for (int g = 0; g < ngroups; g++) std::memset(ls + tg[g] + 16 * n2, 0, 32);
        for (int g = 0; g < ngroups; g++) {
            uint8_t* dst = ls + tg[g];
            const uint8_t* src = ls + verts + uoff + 3 * g;
            uint32_t v = 0;
            auto g3 = [](const uint8_t* p) { return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]; };
            for (; v + 4 <= n2; v += 4) {
                const uint8_t* p = src + stride * v;
                unit_fast2(g3(p), g3(p + stride), g3(p + 2 * stride), g3(p + 3 * stride), ps, dst + 16 * v);
            }
            for (; v < n2; v += 2) {
                const uint8_t* pa = src + stride * v;
                const uint8_t* pb = pa + stride;
                uint32_t ga = (uint32_t(pa[0]) << 16) | (uint32_t(pa[1]) << 8) | pa[2];
                uint32_t gb = (uint32_t(pb[0]) << 16) | (uint32_t(pb[1]) << 8) | pb[2];
                unit_fast(ga, gb, ps, dst + 16 * v, dst + 16 * v + 16);
            }
        }
    } else {
        uint8_t prev[4][16] = {};
        auto g24 = [&](uint32_t a) { return (uint32_t(ls[a & kMask]) << 16) | (uint32_t(ls[(a + 1) & kMask]) << 8) | ls[(a + 2) & kMask]; };
        for (uint32_t k = 0; k <= pairs; k++) {
            uint8_t cur[4][16];
            if (k < pairs) {
                uint32_t va = verts + 2 * stride * k, vb = va + stride;
                uint8_t pa[16], pb[16];
                decode_one(va, pa);
                decode_one(vb, pb);
                for (int g = 0; g < ngroups; g++) {
                    uint32_t ga = g24(va + uoff + 3 * g), gb = g24(vb + uoff + 3 * g);
                    UnitVec u;
                    unit_pair(ga, gb, u);
                    unit_out(u, pats[ga & 15], false, cur[2 * g]);
                    unit_out(u, pats[gb & 15], true, cur[2 * g + 1]);
                }
                uint32_t pa_at = t0 + 32 * k;
                for (int i = 0; i < 16; i++) ls[(pa_at + i) & kMask] = pa[i];
                for (int i = 0; i < 16; i++) ls[(pa_at + 16 + i) & kMask] = pb[i];
            }
            uint32_t at = k == 0 ? 16 * n2 : 32 * (k - 1);
            for (int g = 0; g < ngroups; g++) {
                for (int i = 0; i < 16; i++) ls[(tg[g] + at + i) & kMask] = prev[2 * g][i];
                for (int i = 0; i < 16; i++) ls[(tg[g] + at + 16 + i) & kMask] = prev[2 * g + 1][i];
            }
            if (k < pairs) std::memcpy(prev, cur, sizeof prev);
        }
    }
    if (outs) {
        outs->add(t0, t0 + 16 * n2);
        for (int g = 0; g < ngroups; g++) outs->add(tg[g], tg[g] + 16 * n2);
        outs->add(ctx + 92, ctx + 108);
        outs->add(ctx + 352, ctx + 368);
    }
    return true;
}

}

uint64_t overlay_hash(const uint8_t* p, uint32_t n) { return hash64(p, n); }

int vdecomp_identify(const uint8_t* ls, uint32_t entry) {
    entry &= 0x3fff0;
    uint64_t head;
    std::memcpy(&head, ls + entry, 8);
    uint32_t last = 0;
    uint64_t h = 0;
    for (const Sig& s : kSigs) {
        if (s.head != head || entry + s.size > kLs) continue;
        if (s.size != last) {
            h = hash64(ls + entry, s.size);
            last = s.size;
        }
        if (h == s.hash) return s.id;
    }
    return -1;
}

bool vdecomp_run(uint8_t* ls, int overlay, uint32_t ctx, uint32_t verts, uint32_t nv, uint32_t desc, uint32_t, const VdRegs& regs, Outputs* outs) {
    uint32_t sp = spuf::ld32(regs.r1) & 0x3fff0;
    if (sp + 64 > kLs || sp < 0x400) return false;
    if (overlay != 0 && overlay != 1 && overlay != 3 && overlay != 9 && overlay != 10 && overlay != 11 && overlay != 12) return false;
    ctx &= 0x3fff0;
    if (ctx + 368 > kLs || ctx < 0x10000) return false;
    {
        uint32_t stride = overlay == 10 || overlay == 12 ? 12 : overlay == 9 || overlay == 11 ? 9 : overlay == 0 ? 15 : 18;
        uint32_t n2 = (nv + 1) & ~1u;
        uint64_t lim = sp > 0x1000 ? sp - 0x1000 : 0;
        if (!n2 || verts < 0x10c80 || uint64_t(verts) + uint64_t(stride) * n2 + 16 > lim) return false;
        if (overlay != 0 && overlay != 1 && overlay != 3 && ((desc & 15) || desc < 0x10000 || uint64_t(desc) + 0x40 > lim)) return false;
        int need = overlay == 12 || overlay == 3 ? 3 : overlay == 11 ? 2 : 1;
        uint8_t fl[16];
        std::memcpy(fl, ls + ctx + 352, 16);
        for (int a = 0; a < need; a++) {
            uint32_t k = 16;
            for (uint32_t i = 0; i < 16; i++)
                if (fl[i] == 0) {
                    k = i;
                    break;
                }
            if (k == 16) return false;
            fl[k] = 1;
            uint32_t t = rd32(ls, ctx + 16 + 4 * k);
            if ((t & 15) || t < 0x10c80 || uint64_t(t) + 16ull * n2 + 32 > lim) return false;
        }
    }
    PosFmt pf;
    if (overlay != 0 && overlay != 1 && overlay != 3) {
        pf.load(ls, desc);
        if (!pf.supported()) return false;
    }
    spuf::CsrGuard g;
    uint32_t frame = overlay == 12 ? 384 : 224;
    uint32_t fp = sp - frame;
    std::memcpy(ls + fp, regs.r1, 16);
    for (int i = 0; i < 12; i++) std::memcpy(ls + fp + 32 + 16 * i, kPatterns[i], 16);
    uint8_t spill[64];
    if (overlay == 12) {
        for (int i = 0; i < 10; i++) std::memcpy(ls + sp - 16 * (i + 1), regs.saved[i], 16);
        std::memcpy(spill, ls + sp - 160, 64);
    } else std::memcpy(spill, ls + sp, 64);
    if (overlay == 10) return run_pos(ls, ctx, verts, nv, pf, 12, outs);
    if (overlay == 9) return run_pos(ls, ctx, verts, nv, pf, 9, outs);
    if (overlay == 12) {
        const uint32_t attrs[2] = {3, 4}, slots[2] = {100, 104};
        return run_unit(ls, ctx, verts, nv, pf, spill, 12, 2, attrs, slots, outs);
    }
    if (overlay == 11) {
        const uint32_t attrs[1] = {2}, slots[1] = {96};
        return run_unit(ls, ctx, verts, nv, pf, spill, 9, 1, attrs, slots, outs);
    }
    if (overlay == 3) {
        const uint32_t attrs[2] = {3, 4}, slots[2] = {100, 104};
        return run_unit(ls, ctx, verts, nv, pf, spill, 18, 2, attrs, slots, outs, true, 12);
    }
    if (overlay == 1) return run_copy(ls, ctx, verts, nv, 18, outs);
    return run_copy(ls, ctx, verts, nv, 15, outs);
}

}
