#include "edge_cull.h"

#include "spu_float.h"

#include <cstring>
#include <vector>

namespace edge {

namespace {

constexpr uint32_t kMask = 0x3fff0;

alignas(16) const uint8_t kCompactA[16][16] = {
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f},
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f},
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x12, 0x13, 0x14, 0x15},
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x80, 0x80, 0x80, 0x80},
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15},
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x80, 0x80, 0x80, 0x80},
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x80, 0x80, 0x80, 0x80},
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
    {0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15},
    {0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x80, 0x80, 0x80, 0x80},
    {0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x80, 0x80, 0x80, 0x80},
    {0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
    {0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x80, 0x80, 0x80, 0x80},
    {0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
    {0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
    {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
};

alignas(16) const uint8_t kCompactB[16][16] = {
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
    {0x00, 0x01, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
    {0x06, 0x07, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
    {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
    {0x06, 0x07, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
    {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
    {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
    {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
    {0x06, 0x07, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
    {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
    {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
    {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
    {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
    {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
    {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
    {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
};

inline uint32_t ld32(const uint8_t* ls, uint32_t a) { return spuf::ld32(ls + (a & 0x3fffc)); }
inline float ldf(const uint8_t* ls, uint32_t a) { return spuf::fl(ld32(ls, a)); }
inline uint32_t bits(float f) { return spuf::bits(f); }

inline void put_quad(uint8_t* ls, uint32_t a, const uint8_t* q) { std::memcpy(ls + (a & kMask), q, 16); }

void add_words(uint8_t* out, const uint8_t* in, uint32_t add) {
    for (int w = 0; w < 4; w++) spuf::st32(out + 4 * w, spuf::ld32(in + 4 * w) + add);
}

inline __m128i vkey(__m128 f) {
    __m128i v = _mm_castps_si128(f);
    __m128i mag = _mm_and_si128(v, _mm_set1_epi32(0x7FFFFFFF));
    __m128i zero = _mm_cmpeq_epi32(_mm_and_si128(v, _mm_set1_epi32(0x7F800000)), _mm_setzero_si128());
    mag = _mm_andnot_si128(zero, mag);
    __m128i neg = _mm_srai_epi32(v, 31);
    return _mm_sub_epi32(_mm_xor_si128(mag, neg), neg);
}

inline __m128 vfrest(__m128 x) {
    __m128i r = _mm_castps_si128(_mm_div_ps(_mm_set1_ps(1.0f), x));
    __m128i e = _mm_cmpeq_epi32(_mm_and_si128(r, _mm_set1_epi32(0x7F800000)), _mm_set1_epi32(0x7F800000));
    return _mm_castsi128_ps(_mm_blendv_epi8(r, _mm_or_si128(_mm_and_si128(r, _mm_set1_epi32(int(0x80000000u))), _mm_set1_epi32(0x7FFFFFFF)), e));
}

inline __m128i vcflts(__m128 x) {
    __m128i xi = _mm_castps_si128(x);
    __m128i r = _mm_cvttps_epi32(x);
    __m128i big = _mm_cmpgt_epi32(_mm_and_si128(xi, _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(0x4EFFFFFF));
    __m128i sat = _mm_xor_si128(_mm_set1_epi32(0x7FFFFFFF), _mm_srai_epi32(xi, 31));
    return _mm_blendv_epi8(r, sat, big);
}

inline __m128i vmask(__m128i v, int bit) { return _mm_and_si128(v, _mm_set1_epi32(bit)); }

}

int32_t cull_triangles_ref(uint8_t* ls, const CullIn& in) {
    uint32_t ctx = in.ctx & kMask;
    uint32_t flavor = spuf::ld32(in.r4);
    if (!flavor) return -1;
    if (!(ls[ctx] & 0x80)) return -1;
    if (ld32(ls, ctx + 388)) return -1;
    uint32_t eff = flavor | (ls[ctx + 320] == 5 ? 0x80000000u : 0u);
    if (eff != 1 && eff != 2 && eff != 3) return -1;
    uint32_t nv = spuf::ld16(ls + ctx + 8);
    if (!nv) return -1;
    if (spuf::ld32(in.r5) != 0) return -1;
    spuf::CsrGuard csr;

    uint32_t sp = spuf::ld32(in.r1);
    uint8_t r1p[16], r126p[16];
    add_words(r1p, in.r1, uint32_t(-144));
    spuf::st32(r126p, 0x4000);
    for (int w = 1; w < 4; w++) spuf::st32(r126p + 4 * w, 0xffff69c0u);
    put_quad(ls, sp - 16, in.r80);
    put_quad(ls, sp - 32, in.r81);
    put_quad(ls, sp - 48, in.r82);
    put_quad(ls, sp - 64, in.r126);
    put_quad(ls, sp + 16, in.r0);
    put_quad(ls, sp - 144, in.r1);
    uint32_t spp = sp - 144;
    put_quad(ls, spp - 16, in.r4);
    put_quad(ls, spp - 32, r126p);
    put_quad(ls, spp - 64, r1p);

    float L[3][4], M[4][4], C[4][4];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) L[r][c] = ldf(ls, ctx + 160 + 16 * r + 4 * c);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) M[r][c] = ldf(ls, ctx + 224 + 16 * r + 4 * c);
    const float e3[4] = {0, 0, 0, 1};
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float t = spuf::fm(M[r][0], L[0][c]);
            t = spuf::fma(M[r][1], L[1][c], t);
            t = spuf::fma(M[r][2], L[2][c], t);
            C[r][c] = spuf::fma(M[r][3], e3[c], t);
        }
    float scx = ldf(ls, ctx + 288), scy = ldf(ls, ctx + 292), scz = ldf(ls, ctx + 296);
    float ofx = ldf(ls, ctx + 304), ofy = ldf(ls, ctx + 308), ofz = ldf(ls, ctx + 312);
    uint32_t h0 = spuf::ld16(ls + ctx + 208), h1 = spuf::ld16(ls + ctx + 210), h2 = spuf::ld16(ls + ctx + 212), h3 = spuf::ld16(ls + ctx + 214);
    float x0 = spuf::fl(spuf::cuflt(h0, 155)), y0 = spuf::fl(spuf::cuflt(h1, 155));
    float x1 = spuf::fl(spuf::cuflt((h0 + h2) & 0xffff, 155)), y1 = spuf::fl(spuf::cuflt((h1 + h3) & 0xffff, 155));
    float d0 = ldf(ls, ctx + 216), d1 = ldf(ls, ctx + 220);
    bool dg = spuf::fcgt(d0, d1);
    float zmin = dg ? d1 : d0, zmax = dg ? d0 : d1;

    uint32_t tab = ld32(ls, ctx + 16 + 4 * (ls[ctx] & 15));
    uint32_t pos = ld32(ls, ctx + 92);
    uint32_t vgroups = (nv + 3) >> 2;
    {
        __m128 c[4][4];
        for (int r = 0; r < 4; r++)
            for (int k = 0; k < 4; k++) c[r][k] = _mm_set1_ps(C[r][k]);
        __m128 vscx = _mm_set1_ps(scx), vscy = _mm_set1_ps(scy), vscz = _mm_set1_ps(scz);
        __m128 vofx = _mm_set1_ps(ofx), vofy = _mm_set1_ps(ofy), vofz = _mm_set1_ps(ofz);
        __m128i kx1 = _mm_set1_epi32(spuf::key(bits(x1))), kx0 = _mm_set1_epi32(spuf::key(bits(x0)));
        __m128i ky1 = _mm_set1_epi32(spuf::key(bits(y1))), ky0 = _mm_set1_epi32(spuf::key(bits(y0)));
        __m128i kz1 = _mm_set1_epi32(spuf::key(bits(zmax))), kz0 = _mm_set1_epi32(spuf::key(bits(zmin)));
        __m128 one = _mm_set1_ps(1.0f);
        __m128 sign = _mm_castsi128_ps(_mm_set1_epi32(int(0x80000000u)));
        for (uint32_t g = 0; g < vgroups; g++) {
            uint32_t pa = pos + 64 * g;
            __m128 p0 = spuf::ld4(ls + (pa & kMask)), p1 = spuf::ld4(ls + ((pa + 16) & kMask));
            __m128 p2 = spuf::ld4(ls + ((pa + 32) & kMask)), p3 = spuf::ld4(ls + ((pa + 48) & kMask));
            _MM_TRANSPOSE4_PS(p0, p1, p2, p3);
            __m128 X = p0, Y = p1, Z = p2;
            __m128 W = spuf::fma(Z, c[3][2], spuf::fma(Y, c[3][1], spuf::fma(X, c[3][0], c[3][3])));
            __m128 Xc = spuf::fma(Z, c[0][2], spuf::fma(Y, c[0][1], spuf::fma(X, c[0][0], c[0][3])));
            __m128 Yc = spuf::fma(Z, c[1][2], spuf::fma(Y, c[1][1], spuf::fma(X, c[1][0], c[1][3])));
            __m128 Zc = spuf::fma(Z, c[2][2], spuf::fma(Y, c[2][1], spuf::fma(X, c[2][0], c[2][3])));
            __m128 r = vfrest(W);
            __m128 e = spuf::fnms(r, W, one);
            __m128 rr = spuf::fma(e, r, r);
            __m128 s = _mm_and_ps(W, sign);
            __m128 sx = spuf::fma(_mm_xor_ps(spuf::fm(Xc, rr), s), vscx, vofx);
            __m128 sy = spuf::fma(_mm_xor_ps(spuf::fm(Yc, rr), s), vscy, vofy);
            __m128 sz = spuf::fma(_mm_xor_ps(spuf::fm(Zc, rr), s), vscz, vofz);
            __m128i ksx = vkey(sx), ksy = vkey(sy), ksz = vkey(sz);
            __m128i code = vmask(_mm_cmpgt_epi32(ksx, kx1), 0x80);
            code = _mm_or_si128(code, vmask(_mm_cmpgt_epi32(kx0, ksx), 0x40));
            code = _mm_or_si128(code, vmask(_mm_cmpgt_epi32(ksy, ky1), 0x20));
            code = _mm_or_si128(code, vmask(_mm_cmpgt_epi32(ky0, ksy), 0x10));
            code = _mm_or_si128(code, vmask(_mm_cmpgt_epi32(ksz, kz1), 0x08));
            code = _mm_or_si128(code, vmask(_mm_cmpgt_epi32(kz0, ksz), 0x04));
            code = _mm_or_si128(code, vmask(_mm_cmpgt_epi32(vkey(W), _mm_setzero_si128()), 0x02));
            __m128 q0 = sx, q1 = sy, q2 = _mm_castsi128_ps(code), q3 = _mm_setzero_ps();
            _MM_TRANSPOSE4_PS(q0, q1, q2, q3);
            uint32_t ta = tab + 64 * g;
            spuf::st4(ls + (ta & kMask), q0);
            spuf::st4(ls + ((ta + 16) & kMask), q1);
            spuf::st4(ls + ((ta + 32) & kMask), q2);
            spuf::st4(ls + ((ta + 48) & kMask), q3);
        }
    }

    uint32_t idxflav = ls[ctx + 6] >> 4;
    uint32_t flip = (idxflav == 0 || idxflav == 2) ? 0x80000000u : 0u;
    if (eff == 3) flip ^= 0x80000000u;
    __m128i bfen = _mm_set1_epi32(eff != 1 ? -1 : 0);
    __m128i vflip = _mm_set1_epi32(int(flip));
    uint8_t smp = ls[ctx + 320];
    __m128 pscale = _mm_set1_ps((smp != 0 && uint8_t(smp - 3) <= 1) ? 2.0f : 1.0f);
    __m128 vhalf = _mm_set1_ps(0.5f);

    uint32_t ni = spuf::ld16(ls + ctx + 400);
    uint32_t ntri = uint32_t((uint64_t(ni) * 0xaaaaaaabull) >> 33) & 0xffff;
    uint32_t idx = ld32(ls, ctx + 80);
    uint32_t groups = (ntri + 3) >> 2;
    uint32_t end = idx + ((6 * ntri + 15) & ~15u);
    alignas(16) uint8_t ring[4][32] = {};
    uint8_t mring[4] = {};
    auto load_group = [&](uint32_t g) {
        uint8_t* gd = ring[g & 3];
        uint32_t ga = idx + 24 * g;
        if ((ga & 0x3ffff) + 24 <= 0x40000) std::memcpy(gd, ls + (ga & 0x3ffff), 24);
        else
            for (uint32_t b = 0; b < 24; b++) gd[b] = ls[(ga + b) & 0x3ffff];
    };
    if (groups) load_group(0);
    uint32_t P = idx;
    uint32_t surv = 0;
    for (uint32_t k = 0; k < groups + 2; k++) {
        if (k + 1 < groups) load_group(k + 1);
        if (k < groups) {
            const uint8_t* gd = ring[k & 3];
            alignas(16) float xa[3][4], ya[3][4];
            alignas(16) uint32_t ca[3][4];
            for (int t = 0; t < 4; t++)
                for (int v = 0; v < 3; v++) {
                    const uint8_t* rp = ls + ((tab + 16 * spuf::ld16(gd + t * 6 + 2 * v)) & kMask);
                    xa[v][t] = spuf::ldf(rp);
                    ya[v][t] = spuf::ldf(rp + 4);
                    ca[v][t] = spuf::ld32(rp + 8);
                }
            __m128 xA = _mm_load_ps(xa[0]), xB = _mm_load_ps(xa[1]), xC = _mm_load_ps(xa[2]);
            __m128 yA = _mm_load_ps(ya[0]), yB = _mm_load_ps(ya[1]), yC = _mm_load_ps(ya[2]);
            __m128i cA = _mm_load_si128(reinterpret_cast<const __m128i*>(ca[0]));
            __m128i cB = _mm_load_si128(reinterpret_cast<const __m128i*>(ca[1]));
            __m128i cC = _mm_load_si128(reinterpret_cast<const __m128i*>(ca[2]));
            __m128i orc = _mm_or_si128(_mm_or_si128(cA, cB), cC);
            __m128i andc = _mm_and_si128(_mm_and_si128(cA, cB), cC);
            __m128i zero = _mm_setzero_si128();
            __m128i frustum = _mm_xor_si128(_mm_cmpeq_epi32(vmask(andc, 252), zero), _mm_set1_epi32(-1));
            __m128i front = _mm_andnot_si128(_mm_cmpeq_epi32(vmask(andc, 2), zero), _mm_cmpeq_epi32(vmask(orc, 4), zero));
            __m128 e1x = spuf::fs(xB, xA), e2x = spuf::fs(xC, xA), e1y = spuf::fs(yB, yA), e2y = spuf::fs(yC, yA);
            __m128 cr = spuf::fms(e1x, e2y, spuf::fm(e1y, e2x));
            __m128i back = _mm_cmpgt_epi32(vkey(_mm_castsi128_ps(_mm_xor_si128(_mm_castps_si128(cr), vflip))), zero);
            __m128i pxA = vcflts(spuf::fma(xA, pscale, vhalf)), pxB = vcflts(spuf::fma(xB, pscale, vhalf)), pxC = vcflts(spuf::fma(xC, pscale, vhalf));
            __m128i pyA = vcflts(spuf::fma(yA, pscale, vhalf)), pyB = vcflts(spuf::fma(yB, pscale, vhalf)), pyC = vcflts(spuf::fma(yC, pscale, vhalf));
            __m128i degen = _mm_or_si128(_mm_and_si128(_mm_cmpeq_epi32(pxA, pxB), _mm_cmpeq_epi32(pxB, pxC)), _mm_and_si128(_mm_cmpeq_epi32(pyA, pyB), _mm_cmpeq_epi32(pyB, pyC)));
            __m128i cull = _mm_or_si128(frustum, _mm_and_si128(front, _mm_or_si128(_mm_and_si128(bfen, back), degen)));
            uint32_t lanes = uint32_t(_mm_movemask_ps(_mm_castsi128_ps(cull)));
            uint32_t m = ((lanes & 1) << 3) | ((lanes & 2) << 1) | ((lanes & 4) >> 1) | ((lanes & 8) >> 3);
            uint32_t first = 4 * k;
            if (first + 4 > ntri) m |= 0xfu >> (ntri - first);
            mring[k & 3] = uint8_t(m);
        }
        if (k < 2) {
            uint32_t A = P & ~15u, j = P & 15;
            alignas(16) uint8_t buf[48] = {};
            std::memcpy(buf, ls + (A & kMask), j);
            put_quad(ls, A, buf);
            put_quad(ls, int32_t(P) > int32_t(end - 17) ? spp + 48 : A + 16, buf + 16);
            put_quad(ls, int32_t(P) > int32_t(end - 33) ? spp + 64 : A + 32, buf + 32);
            continue;
        }
        uint32_t g = k - 2;
        uint32_t m = mring[g & 3];
        const uint8_t* src = ring[g & 3];
        __m128i lo = _mm_load_si128(reinterpret_cast<const __m128i*>(src));
        __m128i hi = _mm_and_si128(_mm_load_si128(reinterpret_cast<const __m128i*>(src + 16)), _mm_setr_epi32(-1, -1, 0, 0));
        __m128i ia = _mm_load_si128(reinterpret_cast<const __m128i*>(kCompactA[m]));
        __m128i ib = _mm_load_si128(reinterpret_cast<const __m128i*>(kCompactB[m]));
        __m128i sel = _mm_cmpeq_epi8(_mm_and_si128(ia, _mm_set1_epi8(0x10)), _mm_set1_epi8(0x10));
        alignas(16) uint8_t buf[64];
        uint32_t j = P & 15, A = P & ~15u;
        std::memcpy(buf, ls + (A & kMask), 16);
        alignas(16) uint8_t cp[32];
        _mm_store_si128(reinterpret_cast<__m128i*>(cp), _mm_blendv_epi8(_mm_shuffle_epi8(lo, ia), _mm_shuffle_epi8(hi, ia), sel));
        _mm_store_si128(reinterpret_cast<__m128i*>(cp + 16), _mm_shuffle_epi8(hi, ib));
        std::memcpy(buf + j, cp, 32);
        std::memcpy(buf + j + 32, cp + 16, 16 - j);
        put_quad(ls, A, buf);
        put_quad(ls, int32_t(P) > int32_t(end - 17) ? spp + 48 : A + 16, buf + 16);
        put_quad(ls, int32_t(P) > int32_t(end - 33) ? spp + 64 : A + 32, buf + 32);
        uint32_t s = 4 - uint32_t(__builtin_popcount(m));
        P += 6 * s;
        surv += s;
    }

    uint32_t nout = 3 * surv;
    spuf::st16(ls + ctx + 400, nout & 0xffff);
    spuf::st32(ls + ctx + 88, (idx + 2 * nout + 15) & ~15u);
    return int32_t(nout);
}

namespace {

inline __m256 pz8(__m256 r) {
    __m256i ri = _mm256_castps_si256(r);
    return _mm256_castsi256_ps(_mm256_andnot_si256(_mm256_cmpeq_epi32(ri, _mm256_set1_epi32(int(0x80000000u))), ri));
}

struct Chk {
    __m256 bad = _mm256_setzero_ps();
    inline __m256 c(__m256 r) {
        bad = _mm256_or_ps(bad, _mm256_cmp_ps(_mm256_and_ps(r, _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF))), _mm256_castsi256_ps(_mm256_set1_epi32(0x7F7FFFFF)), _CMP_NLT_UQ));
        return r;
    }
    inline bool ok() const { return _mm256_testz_ps(bad, bad); }
};

inline __m256i vkey8(__m256 f) {
    __m256i v = _mm256_castps_si256(f);
    __m256i mag = _mm256_and_si256(v, _mm256_set1_epi32(0x7FFFFFFF));
    __m256i zero = _mm256_cmpeq_epi32(_mm256_and_si256(v, _mm256_set1_epi32(0x7F800000)), _mm256_setzero_si256());
    mag = _mm256_andnot_si256(zero, mag);
    __m256i neg = _mm256_srai_epi32(v, 31);
    return _mm256_sub_epi32(_mm256_xor_si256(mag, neg), neg);
}

inline __m256 vfrest8(__m256 x) {
    __m256i r = _mm256_castps_si256(_mm256_div_ps(_mm256_set1_ps(1.0f), x));
    __m256i e = _mm256_cmpeq_epi32(_mm256_and_si256(r, _mm256_set1_epi32(0x7F800000)), _mm256_set1_epi32(0x7F800000));
    return _mm256_castsi256_ps(_mm256_blendv_epi8(r, _mm256_or_si256(_mm256_and_si256(r, _mm256_set1_epi32(int(0x80000000u))), _mm256_set1_epi32(0x7FFFFFFF)), e));
}

inline __m256i vcflts8(__m256 x) {
    __m256i xi = _mm256_castps_si256(x);
    __m256i r = _mm256_cvttps_epi32(x);
    __m256i big = _mm256_cmpgt_epi32(_mm256_and_si256(xi, _mm256_set1_epi32(0x7FFFFFFF)), _mm256_set1_epi32(0x4EFFFFFF));
    __m256i sat = _mm256_xor_si256(_mm256_set1_epi32(0x7FFFFFFF), _mm256_srai_epi32(xi, 31));
    return _mm256_blendv_epi8(r, sat, big);
}

inline __m256i absbits8(__m256 v) { return _mm256_and_si256(_mm256_castps_si256(v), _mm256_set1_epi32(0x7FFFFFFF)); }

const __m256i kBswap8 = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);

inline __m256 ld8q(const uint8_t* ls, uint32_t lo, uint32_t hi) {
    __m256i v = _mm256_loadu2_m128i(reinterpret_cast<const __m128i*>(ls + hi), reinterpret_cast<const __m128i*>(ls + lo));
    return _mm256_castsi256_ps(_mm256_shuffle_epi8(v, kBswap8));
}

inline void st8q(uint8_t* ls, uint32_t lo, uint32_t hi, __m256 v) {
    __m256i b = _mm256_shuffle_epi8(_mm256_castps_si256(v), kBswap8);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(ls + lo), _mm256_castsi256_si128(b));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(ls + hi), _mm256_extracti128_si256(b, 1));
}

struct WinMasks {
    alignas(16) uint8_t sl[16][16];
    alignas(16) uint8_t bl[16][16];
    alignas(16) uint8_t ma[16][16];
    alignas(16) uint8_t rot[16][16];
    WinMasks() {
        for (int j = 0; j < 16; j++)
            for (int i = 0; i < 16; i++) {
                sl[j][i] = i < j ? 0x80 : uint8_t(i - j);
                bl[j][i] = i < j ? 0xff : 0x00;
                ma[j][i] = i < j ? uint8_t(16 - j + i) : 0x80;
                rot[j][i] = uint8_t((i - j) & 15);
            }
    }
};

const WinMasks kWin;

struct IdxMasks {
    alignas(16) uint8_t lo[3][16];
    alignas(16) uint8_t hi[3][16];
    IdxMasks() {
        for (int v = 0; v < 3; v++)
            for (int t = 0; t < 4; t++) {
                int o = 6 * t + 2 * v;
                uint8_t* L = lo[v] + 4 * t;
                uint8_t* H = hi[v] + 4 * t;
                L[2] = L[3] = H[2] = H[3] = 0x80;
                L[0] = o + 1 < 16 ? uint8_t(o + 1) : 0x80;
                L[1] = o < 16 ? uint8_t(o) : 0x80;
                H[0] = o + 1 >= 16 ? uint8_t(o + 1 - 16) : 0x80;
                H[1] = o >= 16 ? uint8_t(o - 16) : 0x80;
            }
    }
};

const IdxMasks kIdx;

inline __m128i ldq(const uint8_t* p) { return _mm_loadu_si128(reinterpret_cast<const __m128i*>(p)); }
inline void stq(uint8_t* p, __m128i v) { _mm_storeu_si128(reinterpret_cast<__m128i*>(p), v); }

struct Setup {
    float C[4][4];
    float scx, scy, scz, ofx, ofy, ofz;
    float x0, y0, x1, y1, zmin, zmax;
};

void load_setup(const uint8_t* ls, uint32_t ctx, Setup& s) {
    __m128 L0 = spuf::ld4(ls + ctx + 160), L1 = spuf::ld4(ls + ctx + 176), L2 = spuf::ld4(ls + ctx + 192);
    __m128 e3 = _mm_setr_ps(0, 0, 0, 1);
    for (int r = 0; r < 4; r++) {
        const uint8_t* m = ls + ctx + 224 + 16 * r;
        __m128 t = spuf::fm(_mm_set1_ps(spuf::ldf(m)), L0);
        t = spuf::fma(_mm_set1_ps(spuf::ldf(m + 4)), L1, t);
        t = spuf::fma(_mm_set1_ps(spuf::ldf(m + 8)), L2, t);
        _mm_storeu_ps(s.C[r], spuf::fma(_mm_set1_ps(spuf::ldf(m + 12)), e3, t));
    }
    s.scx = ldf(ls, ctx + 288), s.scy = ldf(ls, ctx + 292), s.scz = ldf(ls, ctx + 296);
    s.ofx = ldf(ls, ctx + 304), s.ofy = ldf(ls, ctx + 308), s.ofz = ldf(ls, ctx + 312);
    uint32_t h0 = spuf::ld16(ls + ctx + 208), h1 = spuf::ld16(ls + ctx + 210), h2 = spuf::ld16(ls + ctx + 212), h3 = spuf::ld16(ls + ctx + 214);
    s.x0 = spuf::fl(spuf::cuflt(h0, 155)), s.y0 = spuf::fl(spuf::cuflt(h1, 155));
    s.x1 = spuf::fl(spuf::cuflt((h0 + h2) & 0xffff, 155)), s.y1 = spuf::fl(spuf::cuflt((h1 + h3) & 0xffff, 155));
    float d0 = ldf(ls, ctx + 216), d1 = ldf(ls, ctx + 220);
    bool dg = spuf::fcgt(d0, d1);
    s.zmin = dg ? d1 : d0, s.zmax = dg ? d0 : d1;
}

bool small_setup(const Setup& S) {
    auto ok = [](float f) { return (spuf::bits(f) & 0x7FFFFFFFu) <= 0x5D7FFFFFu; };
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (!ok(S.C[r][c])) return false;
    return ok(S.scx) && ok(S.scy) && ok(S.scz) && ok(S.ofx) && ok(S.ofy) && ok(S.ofz);
}

struct Rec {
    float x, y;
    int32_t px, py;
};

struct VertexOut {
    Rec* rec;
    int32_t* code;
    bool small = true;
};

inline void note_small(VertexOut& o, __m128 sx, __m128 sy) {
    __m128i m = _mm_or_si128(_mm_cmpgt_epi32(_mm_and_si128(_mm_castps_si128(sx), _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(0x5E7FFFFF)),
                             _mm_cmpgt_epi32(_mm_and_si128(_mm_castps_si128(sy), _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(0x5E7FFFFF)));
    if (!_mm_testz_si128(m, m)) o.small = false;
}

void vertex_group_exact(uint8_t* ls, const Setup& S, uint32_t pos, uint32_t tab, uint32_t g, __m128 pscale, VertexOut& o) {
    __m128 c[4][4];
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) c[r][k] = _mm_set1_ps(S.C[r][k]);
    __m128 one = _mm_set1_ps(1.0f);
    __m128 sign = _mm_castsi128_ps(_mm_set1_epi32(int(0x80000000u)));
    uint32_t pa = pos + 64 * g;
    __m128 p0 = spuf::ld4(ls + (pa & kMask)), p1 = spuf::ld4(ls + ((pa + 16) & kMask));
    __m128 p2 = spuf::ld4(ls + ((pa + 32) & kMask)), p3 = spuf::ld4(ls + ((pa + 48) & kMask));
    _MM_TRANSPOSE4_PS(p0, p1, p2, p3);
    __m128 X = p0, Y = p1, Z = p2;
    __m128 W = spuf::fma(Z, c[3][2], spuf::fma(Y, c[3][1], spuf::fma(X, c[3][0], c[3][3])));
    __m128 Xc = spuf::fma(Z, c[0][2], spuf::fma(Y, c[0][1], spuf::fma(X, c[0][0], c[0][3])));
    __m128 Yc = spuf::fma(Z, c[1][2], spuf::fma(Y, c[1][1], spuf::fma(X, c[1][0], c[1][3])));
    __m128 Zc = spuf::fma(Z, c[2][2], spuf::fma(Y, c[2][1], spuf::fma(X, c[2][0], c[2][3])));
    __m128 r = vfrest(W);
    __m128 e = spuf::fnms(r, W, one);
    __m128 rr = spuf::fma(e, r, r);
    __m128 s = _mm_and_ps(W, sign);
    __m128 sx = spuf::fma(_mm_xor_ps(spuf::fm(Xc, rr), s), _mm_set1_ps(S.scx), _mm_set1_ps(S.ofx));
    __m128 sy = spuf::fma(_mm_xor_ps(spuf::fm(Yc, rr), s), _mm_set1_ps(S.scy), _mm_set1_ps(S.ofy));
    __m128 sz = spuf::fma(_mm_xor_ps(spuf::fm(Zc, rr), s), _mm_set1_ps(S.scz), _mm_set1_ps(S.ofz));
    __m128i ksx = vkey(sx), ksy = vkey(sy), ksz = vkey(sz);
    __m128i code = vmask(_mm_cmpgt_epi32(ksx, _mm_set1_epi32(spuf::key(bits(S.x1)))), 0x80);
    code = _mm_or_si128(code, vmask(_mm_cmpgt_epi32(_mm_set1_epi32(spuf::key(bits(S.x0))), ksx), 0x40));
    code = _mm_or_si128(code, vmask(_mm_cmpgt_epi32(ksy, _mm_set1_epi32(spuf::key(bits(S.y1)))), 0x20));
    code = _mm_or_si128(code, vmask(_mm_cmpgt_epi32(_mm_set1_epi32(spuf::key(bits(S.y0))), ksy), 0x10));
    code = _mm_or_si128(code, vmask(_mm_cmpgt_epi32(ksz, _mm_set1_epi32(spuf::key(bits(S.zmax)))), 0x08));
    code = _mm_or_si128(code, vmask(_mm_cmpgt_epi32(_mm_set1_epi32(spuf::key(bits(S.zmin))), ksz), 0x04));
    code = _mm_or_si128(code, vmask(_mm_cmpgt_epi32(vkey(W), _mm_setzero_si128()), 0x02));
    __m128 vhalf = _mm_set1_ps(0.5f);
    __m128 px = _mm_castsi128_ps(vcflts(spuf::fma(sx, pscale, vhalf)));
    __m128 py = _mm_castsi128_ps(vcflts(spuf::fma(sy, pscale, vhalf)));
    note_small(o, sx, sy);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(o.code + 4 * g), code);
    __m128 a0 = sx, a1 = sy, a2 = px, a3 = py;
    _MM_TRANSPOSE4_PS(a0, a1, a2, a3);
    _mm_storeu_ps(&o.rec[4 * g].x, a0);
    _mm_storeu_ps(&o.rec[4 * g + 1].x, a1);
    _mm_storeu_ps(&o.rec[4 * g + 2].x, a2);
    _mm_storeu_ps(&o.rec[4 * g + 3].x, a3);
    __m128 q0 = sx, q1 = sy, q2 = _mm_castsi128_ps(code), q3 = _mm_setzero_ps();
    _MM_TRANSPOSE4_PS(q0, q1, q2, q3);
    uint32_t ta = tab + 64 * g;
    spuf::st4(ls + (ta & kMask), q0);
    spuf::st4(ls + ((ta + 16) & kMask), q1);
    spuf::st4(ls + ((ta + 32) & kMask), q2);
    spuf::st4(ls + ((ta + 48) & kMask), q3);
}

struct VConst {
    __m256 c[4][4];
    __m256 scx, scy, scz, ofx, ofy, ofz, ps;
    __m256i kx1, kx0, ky1, ky0, kz1, kz0;
    __m256 fx1, fx0, fy1, fy0, fz1, fz0;
};

struct VSoa {
    float* sx;
    float* sy;
    int32_t* code;
    int32_t* px;
    int32_t* py;
    uint8_t* bad;
};

void vertex_compute(const uint8_t* ls, const VConst& K, uint32_t pos, uint32_t npairs, const VSoa& v) {
    const __m256i lim = _mm256_set1_epi32(0x5D7FFFFF);
    const __m256 one = _mm256_set1_ps(1.0f), vhalf = _mm256_set1_ps(0.5f), sgn = _mm256_castsi256_ps(_mm256_set1_epi32(int(0x80000000u)));
    for (uint32_t p = 0; p < npairs; p++) {
        uint32_t pa = pos + 128 * p;
        __m256 v0 = ld8q(ls, pa, pa + 64), v1 = ld8q(ls, pa + 16, pa + 80), v2 = ld8q(ls, pa + 32, pa + 96), v3 = ld8q(ls, pa + 48, pa + 112);
        __m256 t0 = _mm256_unpacklo_ps(v0, v1), t1 = _mm256_unpackhi_ps(v0, v1), t2 = _mm256_unpacklo_ps(v2, v3), t3 = _mm256_unpackhi_ps(v2, v3);
        __m256 X = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(1, 0, 1, 0));
        __m256 Y = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(3, 2, 3, 2));
        __m256 Z = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(1, 0, 1, 0));
        __m256i big = _mm256_or_si256(_mm256_or_si256(_mm256_cmpgt_epi32(absbits8(X), lim), _mm256_cmpgt_epi32(absbits8(Y), lim)), _mm256_cmpgt_epi32(absbits8(Z), lim));
        Chk ck;
        ck.bad = _mm256_castsi256_ps(big);
        auto row = [&](int r) { return _mm256_fmadd_ps(Z, K.c[r][2], _mm256_fmadd_ps(Y, K.c[r][1], _mm256_fmadd_ps(X, K.c[r][0], K.c[r][3]))); };
        __m256 W = pz8(row(3)), Xc = row(0), Yc = row(1), Zc = row(2);
        __m256 r = vfrest8(W);
        __m256 e = ck.c(_mm256_fnmadd_ps(r, W, one));
        __m256 rr = _mm256_fmadd_ps(e, r, r);
        __m256 s = _mm256_and_ps(W, sgn);
        __m256 xn = _mm256_xor_ps(_mm256_mul_ps(Xc, rr), s);
        __m256 yn = _mm256_xor_ps(_mm256_mul_ps(Yc, rr), s);
        __m256 zn = _mm256_xor_ps(_mm256_mul_ps(Zc, rr), s);
        __m256 sx = pz8(ck.c(_mm256_fmadd_ps(xn, K.scx, K.ofx)));
        __m256 sy = pz8(ck.c(_mm256_fmadd_ps(yn, K.scy, K.ofy)));
        __m256 sz = ck.c(_mm256_fmadd_ps(zn, K.scz, K.ofz));
        __m256 fx = _mm256_fmadd_ps(sx, K.ps, vhalf), fy = _mm256_fmadd_ps(sy, K.ps, vhalf);
        auto bit = [](__m256 m, int b) { return _mm256_and_ps(m, _mm256_castsi256_ps(_mm256_set1_epi32(b))); };
        __m256 cf0 = _mm256_or_ps(bit(_mm256_cmp_ps(sx, K.fx1, _CMP_GT_OQ), 0x80), bit(_mm256_cmp_ps(K.fx0, sx, _CMP_GT_OQ), 0x40));
        __m256 cf1 = _mm256_or_ps(bit(_mm256_cmp_ps(sy, K.fy1, _CMP_GT_OQ), 0x20), bit(_mm256_cmp_ps(K.fy0, sy, _CMP_GT_OQ), 0x10));
        __m256 cf2 = _mm256_or_ps(bit(_mm256_cmp_ps(sz, K.fz1, _CMP_GT_OQ), 0x08), bit(_mm256_cmp_ps(K.fz0, sz, _CMP_GT_OQ), 0x04));
        __m256 cf3 = bit(_mm256_cmp_ps(W, _mm256_setzero_ps(), _CMP_GT_OQ), 0x02);
        __m256 sm = _mm256_castsi256_ps(_mm256_or_si256(_mm256_cmpgt_epi32(absbits8(sx), _mm256_set1_epi32(0x5E7FFFFF)), _mm256_cmpgt_epi32(absbits8(sy), _mm256_set1_epi32(0x5E7FFFFF))));
        v.bad[p] = uint8_t((_mm256_movemask_ps(_mm256_or_ps(ck.bad, sm)) ? 1 : 0));
        _mm256_storeu_ps(v.sx + 8 * p, sx);
        _mm256_storeu_ps(v.sy + 8 * p, sy);
        _mm256_storeu_ps(reinterpret_cast<float*>(v.code + 8 * p), _mm256_or_ps(_mm256_or_ps(cf0, cf1), _mm256_or_ps(cf2, cf3)));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v.px + 8 * p), vcflts8(fx));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(v.py + 8 * p), vcflts8(fy));
    }
}

void vertex_emit(uint8_t* ls, uint32_t tab, uint32_t p, const VSoa& v, VertexOut& o) {
    __m256 sx = _mm256_loadu_ps(v.sx + 8 * p), sy = _mm256_loadu_ps(v.sy + 8 * p);
    __m256 cf = _mm256_loadu_ps(reinterpret_cast<const float*>(v.code + 8 * p));
    __m256 px = _mm256_loadu_ps(reinterpret_cast<const float*>(v.px + 8 * p)), py = _mm256_loadu_ps(reinterpret_cast<const float*>(v.py + 8 * p));
    _mm256_storeu_ps(reinterpret_cast<float*>(o.code + 8 * p), cf);
    __m256 z = _mm256_setzero_ps();
    __m256 a0 = _mm256_unpacklo_ps(sx, sy), a1 = _mm256_unpackhi_ps(sx, sy);
    __m256 p0 = _mm256_unpacklo_ps(px, py), p1 = _mm256_unpackhi_ps(px, py);
    __m256 b0 = _mm256_unpacklo_ps(cf, z), b1 = _mm256_unpackhi_ps(cf, z);
    float* rp = &o.rec[8 * p].x;
    _mm256_storeu2_m128(rp + 16, rp, _mm256_shuffle_ps(a0, p0, _MM_SHUFFLE(1, 0, 1, 0)));
    _mm256_storeu2_m128(rp + 20, rp + 4, _mm256_shuffle_ps(a0, p0, _MM_SHUFFLE(3, 2, 3, 2)));
    _mm256_storeu2_m128(rp + 24, rp + 8, _mm256_shuffle_ps(a1, p1, _MM_SHUFFLE(1, 0, 1, 0)));
    _mm256_storeu2_m128(rp + 28, rp + 12, _mm256_shuffle_ps(a1, p1, _MM_SHUFFLE(3, 2, 3, 2)));
    uint32_t ta = tab + 128 * p;
    st8q(ls, ta, ta + 64, _mm256_shuffle_ps(a0, b0, _MM_SHUFFLE(1, 0, 1, 0)));
    st8q(ls, ta + 16, ta + 80, _mm256_shuffle_ps(a0, b0, _MM_SHUFFLE(3, 2, 3, 2)));
    st8q(ls, ta + 32, ta + 96, _mm256_shuffle_ps(a1, b1, _MM_SHUFFLE(1, 0, 1, 0)));
    st8q(ls, ta + 48, ta + 112, _mm256_shuffle_ps(a1, b1, _MM_SHUFFLE(3, 2, 3, 2)));
}

__attribute__((always_inline)) inline void load_slot(const Rec* rec, const uint32_t* id, __m256& x, __m256& y, __m256i& px, __m256i& py) {
    __m256 m0 = _mm256_loadu2_m128(&rec[id[4]].x, &rec[id[0]].x);
    __m256 m1 = _mm256_loadu2_m128(&rec[id[5]].x, &rec[id[1]].x);
    __m256 m2 = _mm256_loadu2_m128(&rec[id[6]].x, &rec[id[2]].x);
    __m256 m3 = _mm256_loadu2_m128(&rec[id[7]].x, &rec[id[3]].x);
    __m256 t0 = _mm256_unpacklo_ps(m0, m1), t1 = _mm256_unpackhi_ps(m0, m1), t2 = _mm256_unpacklo_ps(m2, m3), t3 = _mm256_unpackhi_ps(m2, m3);
    x = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(1, 0, 1, 0));
    y = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(3, 2, 3, 2));
    px = _mm256_castps_si256(_mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(1, 0, 1, 0)));
    py = _mm256_castps_si256(_mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(3, 2, 3, 2)));
}

bool overlaps(uint32_t a, uint32_t al, uint32_t b, uint32_t bl) { return a < b + bl && b < a + al; }

}

int32_t cull_triangles(uint8_t* ls, const CullIn& in) {
    uint32_t ctx = in.ctx & kMask;
    uint32_t flavor = spuf::ld32(in.r4);
    if (!flavor) return -1;
    if (!(ls[ctx] & 0x80)) return -1;
    if (ld32(ls, ctx + 388)) return -1;
    uint32_t eff = flavor | (ls[ctx + 320] == 5 ? 0x80000000u : 0u);
    if (eff != 1 && eff != 2 && eff != 3) return -1;
    uint32_t nv = spuf::ld16(ls + ctx + 8);
    if (!nv) return -1;
    if (spuf::ld32(in.r5) != 0) return -1;
    if (ctx + 512 > 0x40000) return -1;

    uint32_t sp = spuf::ld32(in.r1);
    if ((sp & 15) || sp < 0x400 || sp > 0x3ffc0) return -1;
    uint32_t spp = sp - 144;
    uint32_t tab = ld32(ls, ctx + 16 + 4 * (ls[ctx] & 15));
    uint32_t pos = ld32(ls, ctx + 92);
    uint32_t vgroups = (nv + 3) >> 2;
    uint32_t nvp = 4 * vgroups;
    uint32_t ni = spuf::ld16(ls + ctx + 400);
    uint32_t ntri = uint32_t((uint64_t(ni) * 0xaaaaaaabull) >> 33) & 0xffff;
    uint32_t idx = ld32(ls, ctx + 80);
    uint32_t groups = (ntri + 3) >> 2;
    if ((pos & 15) || (tab & 15) || (idx & 1) || pos >= 0x40000 || tab < 0x10c80 || tab >= 0x40000 || idx < 0x10c80 || idx >= 0x40000) return -1;
    uint32_t end = idx + ((6 * ntri + 15) & ~15u);
    uint32_t vspan = 64 * (vgroups + 1);
    uint64_t whi64 = uint64_t(end > idx + 24 * groups ? end : idx + 24 * groups) + 64;
    if (uint64_t(pos) + vspan > 0x40000 || uint64_t(tab) + vspan > 0x40000 || whi64 > 0x40000) return -1;
    uint32_t wlo = idx & ~15u, whi = uint32_t(whi64);
    if (overlaps(pos, vspan, tab, vspan) || overlaps(tab, vspan, wlo, whi - wlo) || overlaps(tab, vspan, spp - 80, 240) || overlaps(pos, vspan, spp - 80, 240) || overlaps(wlo, whi - wlo, ctx, 512) ||
        overlaps(tab, vspan, ctx, 512) || overlaps(wlo, whi - wlo, spp - 80, 240))
        return -1;
    spuf::CsrGuard csr;

    uint8_t r1p[16], r126p[16];
    add_words(r1p, in.r1, uint32_t(-144));
    spuf::st32(r126p, 0x4000);
    for (int w = 1; w < 4; w++) spuf::st32(r126p + 4 * w, 0xffff69c0u);
    put_quad(ls, sp - 16, in.r80);
    put_quad(ls, sp - 32, in.r81);
    put_quad(ls, sp - 48, in.r82);
    put_quad(ls, sp - 64, in.r126);
    put_quad(ls, sp + 16, in.r0);
    put_quad(ls, sp - 144, in.r1);
    put_quad(ls, spp - 16, in.r4);
    put_quad(ls, spp - 32, r126p);
    put_quad(ls, spp - 64, r1p);

    Setup S;
    load_setup(ls, ctx, S);
    uint8_t smp = ls[ctx + 320];
    float ps = (smp != 0 && uint8_t(smp - 3) <= 1) ? 2.0f : 1.0f;

    thread_local std::vector<Rec> vrec;
    thread_local std::vector<int32_t> vcode;
    if (vrec.size() < nvp + 8) {
        vrec.resize(nvp + 8);
        vcode.resize(nvp + 8);
    }
    VertexOut vo{vrec.data(), vcode.data(), true};
    __m128 ps4 = _mm_set1_ps(ps);
    VConst K;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) K.c[r][c] = _mm256_set1_ps(S.C[r][c]);
    K.scx = _mm256_set1_ps(S.scx), K.scy = _mm256_set1_ps(S.scy), K.scz = _mm256_set1_ps(S.scz);
    K.ofx = _mm256_set1_ps(S.ofx), K.ofy = _mm256_set1_ps(S.ofy), K.ofz = _mm256_set1_ps(S.ofz);
    K.ps = _mm256_set1_ps(ps);
    K.kx1 = _mm256_set1_epi32(spuf::key(bits(S.x1))), K.kx0 = _mm256_set1_epi32(spuf::key(bits(S.x0)));
    K.ky1 = _mm256_set1_epi32(spuf::key(bits(S.y1))), K.ky0 = _mm256_set1_epi32(spuf::key(bits(S.y0)));
    K.kz1 = _mm256_set1_epi32(spuf::key(bits(S.zmax))), K.kz0 = _mm256_set1_epi32(spuf::key(bits(S.zmin)));
    K.fx1 = _mm256_set1_ps(S.x1), K.fx0 = _mm256_set1_ps(S.x0), K.fy1 = _mm256_set1_ps(S.y1), K.fy0 = _mm256_set1_ps(S.y0), K.fz1 = _mm256_set1_ps(S.zmax), K.fz0 = _mm256_set1_ps(S.zmin);
    bool fastv = small_setup(S) && (bits(S.zmin) & 0x7F800000u) != 0x7F800000u && (bits(S.zmax) & 0x7F800000u) != 0x7F800000u;
    uint32_t npairs = fastv ? vgroups / 2 : 0;
    thread_local std::vector<float> s_sx, s_sy;
    thread_local std::vector<int32_t> s_code, s_px, s_py;
    thread_local std::vector<uint8_t> s_bad;
    if (s_sx.size() < 8 * size_t(npairs) + 8) {
        size_t n = 8 * size_t(npairs) + 8;
        s_sx.resize(n);
        s_sy.resize(n);
        s_code.resize(n);
        s_px.resize(n);
        s_py.resize(n);
        s_bad.resize(npairs + 8);
    }
    VSoa soa{s_sx.data(), s_sy.data(), s_code.data(), s_px.data(), s_py.data(), s_bad.data()};
    vertex_compute(ls, K, pos, npairs, soa);
    for (uint32_t p = 0; p < npairs; p++) {
        if (s_bad[p] & 1) {
            vertex_group_exact(ls, S, pos, tab, 2 * p, ps4, vo);
            vertex_group_exact(ls, S, pos, tab, 2 * p + 1, ps4, vo);
        } else {
            if (s_bad[p] & 2) vo.small = false;
            vertex_emit(ls, tab, p, soa, vo);
        }
    }
    for (uint32_t g = 2 * npairs; g < vgroups; g++) vertex_group_exact(ls, S, pos, tab, g, ps4, vo);
    uint32_t idxflav = ls[ctx + 6] >> 4;
    uint32_t flip = (idxflav == 0 || idxflav == 2) ? 0x80000000u : 0u;
    if (eff == 3) flip ^= 0x80000000u;
    __m128i bfen = _mm_set1_epi32(eff != 1 ? -1 : 0);
    __m128i vflip = _mm_set1_epi32(int(flip));
    __m128 pscale = ps4;
    __m128 vhalf = _mm_set1_ps(0.5f);

    thread_local std::vector<uint8_t> ib;
    thread_local std::vector<uint8_t> masks;
    if (ib.size() < 24 * size_t(groups) + 64) ib.resize(24 * size_t(groups) + 64);
    if (masks.size() < groups + 2) masks.resize(groups + 2);
    std::memcpy(ib.data(), ls + idx, 24 * size_t(groups) + 8);

    auto group_exact = [&](uint32_t k) -> bool {
        const uint8_t* gd = ib.data() + 24 * k;
        alignas(16) float xa[3][4], ya[3][4];
        alignas(16) uint32_t ca[3][4];
        for (int t = 0; t < 4; t++)
            for (int v = 0; v < 3; v++) {
                uint32_t ra = (tab + 16 * spuf::ld16(gd + t * 6 + 2 * v)) & kMask;
                if (4 * k + uint32_t(t) < ntri && (overlaps(ra, 16, wlo, whi - wlo) || overlaps(ra, 16, spp + 48, 32))) return false;
                const uint8_t* rp = ls + ra;
                xa[v][t] = spuf::ldf(rp);
                ya[v][t] = spuf::ldf(rp + 4);
                ca[v][t] = spuf::ld32(rp + 8);
            }
        __m128 xA = _mm_load_ps(xa[0]), xB = _mm_load_ps(xa[1]), xC = _mm_load_ps(xa[2]);
        __m128 yA = _mm_load_ps(ya[0]), yB = _mm_load_ps(ya[1]), yC = _mm_load_ps(ya[2]);
        __m128i cA = _mm_load_si128(reinterpret_cast<const __m128i*>(ca[0]));
        __m128i cB = _mm_load_si128(reinterpret_cast<const __m128i*>(ca[1]));
        __m128i cC = _mm_load_si128(reinterpret_cast<const __m128i*>(ca[2]));
        __m128i orc = _mm_or_si128(_mm_or_si128(cA, cB), cC);
        __m128i andc = _mm_and_si128(_mm_and_si128(cA, cB), cC);
        __m128i zero = _mm_setzero_si128();
        __m128i frustum = _mm_xor_si128(_mm_cmpeq_epi32(vmask(andc, 252), zero), _mm_set1_epi32(-1));
        __m128i front = _mm_andnot_si128(_mm_cmpeq_epi32(vmask(andc, 2), zero), _mm_cmpeq_epi32(vmask(orc, 4), zero));
        __m128 e1x = spuf::fs(xB, xA), e2x = spuf::fs(xC, xA), e1y = spuf::fs(yB, yA), e2y = spuf::fs(yC, yA);
        __m128 cr = spuf::fms(e1x, e2y, spuf::fm(e1y, e2x));
        __m128i back = _mm_cmpgt_epi32(vkey(_mm_castsi128_ps(_mm_xor_si128(_mm_castps_si128(cr), vflip))), zero);
        __m128i pxA = vcflts(spuf::fma(xA, pscale, vhalf)), pxB = vcflts(spuf::fma(xB, pscale, vhalf)), pxC = vcflts(spuf::fma(xC, pscale, vhalf));
        __m128i pyA = vcflts(spuf::fma(yA, pscale, vhalf)), pyB = vcflts(spuf::fma(yB, pscale, vhalf)), pyC = vcflts(spuf::fma(yC, pscale, vhalf));
        __m128i degen = _mm_or_si128(_mm_and_si128(_mm_cmpeq_epi32(pxA, pxB), _mm_cmpeq_epi32(pxB, pxC)), _mm_and_si128(_mm_cmpeq_epi32(pyA, pyB), _mm_cmpeq_epi32(pyB, pyC)));
        __m128i cull = _mm_or_si128(frustum, _mm_and_si128(front, _mm_or_si128(_mm_and_si128(bfen, back), degen)));
        uint32_t lanes = uint32_t(_mm_movemask_ps(_mm_castsi128_ps(cull)));
        uint32_t m = ((lanes & 1) << 3) | ((lanes & 2) << 1) | ((lanes & 4) >> 1) | ((lanes & 8) >> 3);
        uint32_t first = 4 * k;
        if (first + 4 > ntri) m |= 0xfu >> (ntri - first);
        masks[k] = uint8_t(m);
        return true;
    };

    __m256i bfen8 = _mm256_set1_epi32(eff != 1 ? -1 : 0);
    __m256i vflip8 = _mm256_set1_epi32(int(flip));
    const Rec* rec = vrec.data();
    const int32_t* codes = vcode.data();
    uint32_t k = 0;
    if (vo.small) {
        for (; k + 2 <= groups; k += 2) {
            const uint8_t* gd = ib.data() + 24 * k;
            alignas(32) uint32_t ia[3][8];
            uint32_t live = ntri - 4 * k;
            __m128i h0l = ldq(gd), h0h = ldq(gd + 16), h1l = ldq(gd + 24), h1h = ldq(gd + 40);
            __m256i lv = _mm256_cmpgt_epi32(_mm256_set1_epi32(int(live)), _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
            __m256i oorv = _mm256_setzero_si256();
            for (int v = 0; v < 3; v++) {
                __m128i a = _mm_or_si128(_mm_shuffle_epi8(h0l, ldq(kIdx.lo[v])), _mm_shuffle_epi8(h0h, ldq(kIdx.hi[v])));
                __m128i b = _mm_or_si128(_mm_shuffle_epi8(h1l, ldq(kIdx.lo[v])), _mm_shuffle_epi8(h1h, ldq(kIdx.hi[v])));
                __m256i x = _mm256_and_si256(_mm256_set_m128i(b, a), lv);
                oorv = _mm256_or_si256(oorv, _mm256_cmpgt_epi32(x, _mm256_set1_epi32(int(nvp) - 1)));
                _mm256_store_si256(reinterpret_cast<__m256i*>(ia[v]), x);
            }
            if (!_mm256_testz_si256(oorv, oorv)) {
                if (!group_exact(k) || !group_exact(k + 1)) return -1;
                continue;
            }
            __m256 xA, yA, xB, yB, xC, yC;
            __m256i pxA, pyA, pxB, pyB, pxC, pyC;
            load_slot(rec, ia[0], xA, yA, pxA, pyA);
            load_slot(rec, ia[1], xB, yB, pxB, pyB);
            load_slot(rec, ia[2], xC, yC, pxC, pyC);
            __m256i cA = _mm256_i32gather_epi32(codes, _mm256_load_si256(reinterpret_cast<const __m256i*>(ia[0])), 4);
            __m256i cB = _mm256_i32gather_epi32(codes, _mm256_load_si256(reinterpret_cast<const __m256i*>(ia[1])), 4);
            __m256i cC = _mm256_i32gather_epi32(codes, _mm256_load_si256(reinterpret_cast<const __m256i*>(ia[2])), 4);
            __m256 e1x = _mm256_sub_ps(xB, xA), e2x = _mm256_sub_ps(xC, xA), e1y = _mm256_sub_ps(yB, yA), e2y = _mm256_sub_ps(yC, yA);
            __m256 cr = _mm256_fmsub_ps(e1x, e2y, _mm256_mul_ps(e1y, e2x));
            __m256i orc = _mm256_or_si256(_mm256_or_si256(cA, cB), cC);
            __m256i andc = _mm256_and_si256(_mm256_and_si256(cA, cB), cC);
            __m256i zero = _mm256_setzero_si256();
            __m256i frustum = _mm256_xor_si256(_mm256_cmpeq_epi32(_mm256_and_si256(andc, _mm256_set1_epi32(252)), zero), _mm256_set1_epi32(-1));
            __m256i front = _mm256_andnot_si256(_mm256_cmpeq_epi32(_mm256_and_si256(andc, _mm256_set1_epi32(2)), zero), _mm256_cmpeq_epi32(_mm256_and_si256(orc, _mm256_set1_epi32(4)), zero));
            __m256i back = _mm256_castps_si256(_mm256_cmp_ps(_mm256_xor_ps(cr, _mm256_castsi256_ps(vflip8)), _mm256_setzero_ps(), _CMP_GT_OQ));
            __m256i degen = _mm256_or_si256(_mm256_and_si256(_mm256_cmpeq_epi32(pxA, pxB), _mm256_cmpeq_epi32(pxB, pxC)), _mm256_and_si256(_mm256_cmpeq_epi32(pyA, pyB), _mm256_cmpeq_epi32(pyB, pyC)));
            __m256i cull = _mm256_or_si256(frustum, _mm256_and_si256(front, _mm256_or_si256(_mm256_and_si256(bfen8, back), degen)));
            uint32_t lanes = uint32_t(_mm256_movemask_ps(_mm256_castsi256_ps(cull)));
            if (live < 8) lanes |= 0xffu << live;
            static const uint8_t rev4[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
            masks[k] = rev4[lanes & 15];
            masks[k + 1] = rev4[(lanes >> 4) & 15];
        }
    }

    for (; k < groups; k++)
        if (!group_exact(k)) return -1;

    uint32_t P = idx;
    uint32_t surv = 0;
    __m128i zero16 = _mm_setzero_si128();
    for (uint32_t w = 0; w < 2; w++) {
        uint32_t j = P & 15, A = P & ~15u;
        __m128i orig = ldq(ls + A);
        stq(ls + A, _mm_and_si128(orig, ldq(kWin.bl[j])));
        stq(ls + (int32_t(P) > int32_t(end - 17) ? spp + 48 : A + 16), zero16);
        stq(ls + (int32_t(P) > int32_t(end - 33) ? spp + 64 : A + 32), zero16);
    }
    const __m128i lomask = _mm_setr_epi32(-1, -1, 0, 0);
    const __m128i bit10 = _mm_set1_epi8(0x10);
    for (uint32_t gg = 0; gg < groups; gg++) {
        uint32_t m = masks[gg];
        const uint8_t* src = ib.data() + 24 * gg;
        __m128i lo = ldq(src);
        __m128i hi = _mm_and_si128(ldq(src + 16), lomask);
        __m128i ia = _mm_load_si128(reinterpret_cast<const __m128i*>(kCompactA[m]));
        __m128i ibm = _mm_load_si128(reinterpret_cast<const __m128i*>(kCompactB[m]));
        __m128i sel = _mm_cmpeq_epi8(_mm_and_si128(ia, bit10), bit10);
        __m128i cl = _mm_blendv_epi8(_mm_shuffle_epi8(lo, ia), _mm_shuffle_epi8(hi, ia), sel);
        __m128i ch = _mm_shuffle_epi8(hi, ibm);
        uint32_t j = P & 15, A = P & ~15u;
        __m128i orig = ldq(ls + A);
        __m128i q1 = _mm_or_si128(_mm_and_si128(orig, ldq(kWin.bl[j])), _mm_shuffle_epi8(cl, ldq(kWin.sl[j])));
        __m128i q2 = _mm_or_si128(_mm_shuffle_epi8(cl, ldq(kWin.ma[j])), _mm_shuffle_epi8(ch, ldq(kWin.sl[j])));
        __m128i q3 = _mm_shuffle_epi8(ch, ldq(kWin.rot[j]));
        stq(ls + A, q1);
        stq(ls + (int32_t(P) > int32_t(end - 17) ? spp + 48 : A + 16), q2);
        stq(ls + (int32_t(P) > int32_t(end - 33) ? spp + 64 : A + 32), q3);
        uint32_t s = 4 - uint32_t(__builtin_popcount(m));
        P += 6 * s;
        surv += s;
    }

    uint32_t nout = 3 * surv;
    spuf::st16(ls + ctx + 400, nout & 0xffff);
    spuf::st32(ls + ctx + 88, (idx + 2 * nout + 15) & ~15u);
    return int32_t(nout);
}

}
