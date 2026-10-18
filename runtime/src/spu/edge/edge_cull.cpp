#include "edge_cull.h"

#include "spu_float.h"

#include <cstring>

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

}
