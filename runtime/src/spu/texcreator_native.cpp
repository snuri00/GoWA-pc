#include "texcreator_native.h"

#include <immintrin.h>

#include <bit>
#include <cmath>
#include <cstring>
#include <vector>

namespace texcreator {

namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
void put_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

inline uint32_t bits(float f) { return std::bit_cast<uint32_t>(f); }
inline float fl(uint32_t u) { return std::bit_cast<float>(u); }

inline double sfx_to_double(uint32_t b) {
    uint32_t e = (b >> 23) & 0xFF;
    if (!e) return 0.0;
    uint64_t d = (uint64_t(b >> 31) << 63) | (uint64_t(e - 127 + 1023) << 52) | (uint64_t(b & 0x7FFFFF) << 29);
    return std::bit_cast<double>(d);
}

inline uint32_t sfx_from_double(double v) {
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

inline double sd(float a) { return sfx_to_double(bits(a)); }
inline bool special(float r) { return (bits(r) & 0x7FFFFFFFu) >= 0x7F7FFFFFu; }
inline float pz(float r) { return r == 0.0f ? 0.0f : r; }
__attribute__((noinline, cold)) float slow_fa(float a, float b) { return fl(sfx_from_double(sd(a) + sd(b))); }
__attribute__((noinline, cold)) float slow_fs(float a, float b) { return fl(sfx_from_double(sd(a) - sd(b))); }
__attribute__((noinline, cold)) float slow_fm(float a, float b) { return fl(sfx_from_double(sd(a) * sd(b))); }
__attribute__((noinline, cold)) float slow_fma(float a, float b, float c) { return fl(sfx_from_double(std::fma(sd(a), sd(b), sd(c)))); }
inline float fa(float a, float b) {
    float r = a + b;
    if (special(r)) r = slow_fa(a, b);
    return pz(r);
}
inline float fs(float a, float b) {
    float r = a - b;
    if (special(r)) r = slow_fs(a, b);
    return pz(r);
}
inline float fm(float a, float b) {
    float r = a * b;
    if (special(r)) r = slow_fm(a, b);
    return pz(r);
}
inline float fmadd(float a, float b, float c) {
    float r = _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(c)));
    if (special(r)) r = slow_fma(a, b, c);
    return pz(r);
}
inline float fmsub(float a, float b, float c) {
    float r = _mm_cvtss_f32(_mm_fmsub_ss(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(c)));
    if (special(r)) r = slow_fma(a, b, fl(bits(c) ^ 0x80000000u));
    return pz(r);
}
inline float fnmsub(float a, float b, float c) {
    float r = _mm_cvtss_f32(_mm_fnmadd_ss(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(c)));
    if (special(r)) r = slow_fma(fl(bits(a) ^ 0x80000000u), b, c);
    return pz(r);
}
inline float neg(float a) { return fl(bits(a) ^ 0x80000000u); }
inline int32_t key(float a) {
    uint32_t b = bits(a);
    int32_t mag = (b & 0x7F800000u) ? int32_t(b & 0x7FFFFFFFu) : 0;
    return (b & 0x80000000u) ? -mag : mag;
}
inline bool gt(float a, float b) { return key(a) > key(b); }

inline float clampmax(float a) {
    uint32_t b = bits(a);
    if ((b & 0x7F800000u) == 0x7F800000u) return fl((b & 0x80000000u) | 0x7FFFFFFFu);
    return a;
}

inline float recip(float a) {
    uint32_t b = bits(a);
    if ((b & 0x7F800000u) == 0) return fl((b & 0x80000000u) | 0x7FFFFFFFu);
    return clampmax(1.0f / a);
}

inline float rsqrt(float a) {
    uint32_t b = bits(a) & 0x7FFFFFFFu;
    if ((b & 0x7F800000u) == 0) return fl(0x7FFFFFFFu);
    return clampmax(1.0f / std::sqrt(fl(b)));
}

inline int32_t cflts(float a) {
    double v = sfx_to_double(bits(a));
    return v >= 2147483647.0 ? INT32_MAX : v <= -2147483648.0 ? INT32_MIN : int32_t(v);
}

inline uint32_t cfltu(float a) {
    double v = sfx_to_double(bits(a));
    return v >= 4294967295.0 ? UINT32_MAX : v <= 0.0 ? 0 : uint32_t(v);
}

inline float int_to_float(double v) { return fl(sfx_from_double(v)); }

inline float csflt(int32_t i) { return int_to_float(double(i)); }
inline float cuflt(uint32_t u, int scale_log2 = 0) { return int_to_float(std::ldexp(double(u), scale_log2)); }

inline float divide(float num, float den) {
    float r = recip(den);
    float t = fm(num, r);
    float e = fnmsub(t, den, num);
    return fmadd(e, r, t);
}

inline uint16_t to_half(float x) {
    uint32_t b = bits(fm(x, fl(0x07800000u)));
    return uint16_t((((b << 3) & 0xFFFFFFFFu) | (b & 0x80000000u)) >> 16);
}

inline float clamp01(float x) {
    if (!gt(x, 0.0f)) return 0.0f;
    return gt(1.0f, x) ? x : 1.0f;
}

struct V4 {
    float v[4];
    float& operator[](int i) { return v[i]; }
    float operator[](int i) const { return v[i]; }
};

V4 vfa(const V4& a, const V4& b) { return {fa(a[0], b[0]), fa(a[1], b[1]), fa(a[2], b[2]), fa(a[3], b[3])}; }
V4 vfs(const V4& a, const V4& b) { return {fs(a[0], b[0]), fs(a[1], b[1]), fs(a[2], b[2]), fs(a[3], b[3])}; }
V4 vfm(const V4& a, const V4& b) { return {fm(a[0], b[0]), fm(a[1], b[1]), fm(a[2], b[2]), fm(a[3], b[3])}; }
V4 vfms(const V4& a, float s) { return {fm(a[0], s), fm(a[1], s), fm(a[2], s), fm(a[3], s)}; }
V4 vfmadd(const V4& a, float s, const V4& c) { return {fmadd(a[0], s, c[0]), fmadd(a[1], s, c[1]), fmadd(a[2], s, c[2]), fmadd(a[3], s, c[3])}; }

using M4 = V4[4];

V4 combine(const V4* rows, const V4& coef) {
    V4 r = vfms(rows[0], coef[0]);
    r = vfmadd(rows[1], coef[1], r);
    r = vfmadd(rows[2], coef[2], r);
    return vfmadd(rows[3], coef[3], r);
}

void left_mul(const M4& a, M4& m) {
    M4 r;
    for (int j = 0; j < 4; j++) r[j] = combine(m, a[j]);
    for (int j = 0; j < 4; j++) m[j] = r[j];
}

void right_mul(M4& t, const M4& b) {
    M4 r;
    for (int j = 0; j < 4; j++) r[j] = combine(b, t[j]);
    for (int j = 0; j < 4; j++) t[j] = r[j];
}

struct SinCos {
    float s;
    float c;
};

SinCos spu_sincos(float x) {
    float q0 = fm(x, fl(0x3f22f983u));
    float h = fl((bits(q0) & 0x80000000u) | 0x3f000000u);
    int32_t qi = cflts(fa(q0, h));
    float qf = csflt(qi);
    float r = fnmsub(qf, fl(0x3fc90fd9u), x);
    r = fnmsub(qf, fl(0x33a22168u), r);
    float z = fm(r, r);
    float c = fmadd(fmadd(fmadd(z, fl(0xbab24992u), fl(0x3d2aa035u)), z, fl(0xbeffffdfu)), z, 1.0f);
    float s = fmadd(fmadd(fmadd(z, fl(0xb94c8c6du), fl(0x3c088342u)), z, fl(0xbe2aaaa1u)), fm(z, r), r);
    uint32_t qa = uint32_t(qi) & 3, qb = qa + 1;
    float sv = (qa & 1) == 0 ? s : c;
    if (qa & 2) sv = neg(sv);
    float cv = (qb & 1) == 0 ? s : c;
    if (qb & 2) cv = neg(cv);
    return {sv, cv};
}

const uint8_t kPerm[256] = {
    151, 160, 137, 91, 90, 15, 131, 13, 201, 95, 96, 53, 194, 233, 7, 225, 140, 36, 103, 30, 69, 142, 8, 99, 37, 240, 21, 10, 23, 190, 6, 148,
    247, 120, 234, 75, 0, 26, 197, 62, 94, 252, 219, 203, 117, 35, 11, 32, 57, 177, 33, 88, 237, 149, 56, 87, 174, 20, 125, 136, 171, 168, 68, 175,
    74, 165, 71, 134, 139, 48, 27, 166, 77, 146, 158, 231, 83, 111, 229, 122, 60, 211, 133, 230, 220, 105, 92, 41, 55, 46, 245, 40, 244, 102, 143, 54,
    65, 25, 63, 161, 1, 216, 80, 73, 209, 76, 132, 187, 208, 89, 18, 169, 200, 196, 135, 130, 116, 188, 159, 86, 164, 100, 109, 198, 173, 186, 3, 64,
    52, 217, 226, 250, 124, 123, 5, 202, 38, 147, 118, 126, 255, 82, 85, 212, 207, 206, 59, 227, 47, 16, 58, 17, 182, 189, 28, 42, 223, 183, 170, 213,
    119, 248, 152, 2, 44, 154, 163, 70, 221, 153, 101, 155, 167, 43, 172, 9, 129, 22, 39, 253, 19, 98, 108, 110, 79, 113, 224, 232, 178, 185, 112, 104,
    218, 246, 97, 228, 251, 34, 242, 193, 238, 210, 144, 12, 191, 179, 162, 241, 81, 51, 145, 235, 249, 14, 239, 107, 49, 192, 214, 31, 181, 199, 106, 157,
    184, 84, 204, 176, 115, 121, 50, 45, 127, 4, 150, 254, 138, 236, 205, 93, 222, 114, 67, 29, 24, 72, 243, 141, 128, 195, 78, 66, 215, 61, 156, 180,
};

const int8_t kGrad[16][3] = {
    {1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1}, {-1, 0, 1}, {1, 0, -1}, {-1, 0, -1},
    {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}, {0, 1, 0}, {0, -1, 1}, {0, 1, 0}, {0, -1, -1},
};

inline uint32_t perm(uint32_t i) { return kPerm[i & 255]; }

inline float trunc_bits(float x) {
    uint32_t b = bits(x);
    int32_t e = int32_t((b >> 23) & 0xFF);
    if (e <= 126) return 0.0f;
    int32_t sh = e - 127;
    uint32_t mask = sh >= 31 ? 0u : (0x7FFFFFu >> sh);
    return fl(b & ~mask);
}

inline float grad(uint32_t h, float x, float y, float z) {
    const int8_t* g = kGrad[h & 15];
    float tx = fm(x, float(g[0]));
    float ty = fm(y, float(g[1]));
    float tz = fm(z, float(g[2]));
    return fa(fa(tx, ty), tz);
}

inline float lerp(float a, float b, float t) { return fmadd(fs(b, a), t, a); }

float noise3(float px, float py, float pz_) {
    float p[3] = {px, py, pz_};
    float f[3], u[3];
    uint32_t c[3];
    for (int k = 0; k < 3; k++) {
        float s = (bits(p[k]) & 0x80000000u) ? fl(0x3f7fffffu) : 0.0f;
        float t = trunc_bits(fs(p[k], s));
        c[k] = uint32_t(cflts(t)) & 255;
        f[k] = fs(p[k], t);
        float f3 = fm(fm(f[k], f[k]), f[k]);
        float poly = fmadd(f[k], fmsub(f[k], 6.0f, 15.0f), 10.0f);
        u[k] = fm(f3, poly);
    }
    uint32_t A = perm(c[0]) + c[1], AA = perm(A) + c[2], AB = perm(A + 1) + c[2];
    uint32_t B = perm(c[0] + 1) + c[1], BA = perm(B) + c[2], BB = perm(B + 1) + c[2];
    float x0 = f[0], y0 = f[1], z0 = f[2];
    float x1 = fs(x0, 1.0f), y1 = fs(y0, 1.0f), z1 = fs(z0, 1.0f);
    float g000 = grad(perm(AA), x0, y0, z0);
    float g100 = grad(perm(BA), x1, y0, z0);
    float g010 = grad(perm(AB), x0, y1, z0);
    float g110 = grad(perm(BB), x1, y1, z0);
    float g001 = grad(perm(AA + 1), x0, y0, z1);
    float g101 = grad(perm(BA + 1), x1, y0, z1);
    float g011 = grad(perm(AB + 1), x0, y1, z1);
    float g111 = grad(perm(BB + 1), x1, y1, z1);
    float x00 = lerp(g000, g100, u[0]);
    float x10 = lerp(g010, g110, u[0]);
    float x01 = lerp(g001, g101, u[0]);
    float x11 = lerp(g011, g111, u[0]);
    float yy0 = lerp(x00, x10, u[1]);
    float yy1 = lerp(x01, x11, u[1]);
    return lerp(yy0, yy1, u[2]);
}

struct Params {
    float w[80];
    V4 vec(int off) const { return {w[off / 4], w[off / 4 + 1], w[off / 4 + 2], w[off / 4 + 3]}; }
};

void build_row_colours(Params& P, uint8_t* param_be, uint32_t height, std::vector<V4>& table) {
    float y = P.w[0x24 / 4];
    if (!gt(y, -1.0f)) y = -1.0f;
    else if (!gt(1.0f, y)) y = 1.0f;
    float ay = fl(bits(y) & 0x7FFFFFFFu);
    float om = fs(1.0f, ay);
    float p1 = fmadd(ay, fl(0xbaa57a2cu), fl(0x3bda90c5u));
    float p2 = fmadd(ay, fl(0xbd4d8392u), fl(0x3db63a9eu));
    float rs = rsqrt(om);
    float p1b = fmadd(p1, ay, fl(0xbc8bfc66u));
    float p2b = fmadd(p2, ay, fl(0xbe5bbfcau));
    float y2 = fm(ay, ay);
    float sq = fm(rs, om);
    float p2c = fmadd(p2b, ay, fl(0x3fc90fdau));
    float err = fnmsub(rs, sq, fl(0x3f800001u));
    float p1c = fmadd(p1b, ay, fl(0x3cfd10f8u));
    float hs = fm(sq, 0.5f);
    float y4 = fm(y2, y2);
    float sqr = fmadd(err, hs, sq);
    float poly = fmadd(p1c, y4, p2c);
    float half_b = fm(P.w[0x44 / 4], 0.5f);
    float acos_pos = fm(sqr, poly);
    float acos_neg = fnmsub(sqr, poly, fl(0x40490fdau));
    float A = (bits(y) & 0x80000000u) ? acos_neg : acos_pos;
    if (gt(0.0f, A)) A = neg(A);
    float npi = fl(0xc0490fdau);
    float lo = fa(divide(fa(A, half_b), npi), 1.0f);
    float hi = fa(divide(fs(A, half_b), npi), 1.0f);

    float k0 = P.w[0xe0 / 4], k1o = P.w[0xe4 / 4], k2o = P.w[0xe8 / 4];
    float k1 = gt(k0, k1o) ? k0 : k1o;
    float k2 = gt(k1, k2o) ? k1 : k2o;
    P.w[0xe4 / 4] = k1;
    P.w[0xe8 / 4] = k2;
    put_be32(param_be + 0xe4, bits(k1));
    put_be32(param_be + 0xe8, bits(k2));

    V4 C0 = P.vec(0xf0), C1 = P.vec(0x100), C2 = P.vec(0x110);
    float f[8] = {0.0f};
    V4 col[8];
    int last = 1;
    if (gt(k0, lo)) {
        col[0] = C0;
        col[1] = C0;
        last = 1;
        if (!gt(k0, hi)) {
            float range = fs(hi, lo);
            f[1] = divide(fs(k0, lo), range);
            if (gt(k1, hi)) {
                float t = divide(fs(hi, k0), fs(k1, k0));
                col[2] = vfmadd(vfs(C1, C0), t, C0);
                last = 2;
            } else {
                f[2] = divide(fs(k1, lo), range);
                col[2] = C1;
                if (gt(k2, hi)) {
                    float t = divide(fs(hi, k1), fs(k2, k1));
                    col[3] = vfmadd(vfs(C2, C1), t, C1);
                    last = 3;
                } else {
                    f[3] = divide(fs(k2, lo), range);
                    col[3] = C2;
                    col[4] = C2;
                    last = 4;
                }
            }
        }
    } else if (gt(hi, k2)) {
        if (!gt(lo, k1)) {
            float range = fs(hi, lo);
            float t = divide(fs(lo, k0), fs(k1, k0));
            f[1] = divide(fs(k1, lo), range);
            f[2] = divide(fs(k2, lo), range);
            col[0] = vfmadd(vfs(C1, C0), t, C0);
            col[1] = C1;
            col[2] = C2;
            col[3] = C2;
            last = 3;
        } else if (!gt(lo, k2)) {
            float t = divide(fs(lo, k1), fs(k2, k1));
            f[1] = divide(fs(k2, lo), fs(hi, lo));
            col[0] = vfmadd(vfs(C2, C1), t, C1);
            col[1] = C2;
            col[2] = C2;
            last = 2;
        } else {
            col[0] = C2;
            col[1] = C2;
            last = 1;
        }
    } else if (gt(lo, k1)) {
        float d = fs(k2, k1);
        float th = divide(fs(hi, k1), d);
        float tl = divide(fs(lo, k1), d);
        V4 diff = vfs(C2, C1);
        col[1] = vfmadd(diff, th, C1);
        col[0] = vfmadd(diff, tl, C1);
        last = 1;
    } else if (gt(k1, hi)) {
        float d = fs(k1, k0);
        float th = divide(fs(hi, k0), d);
        float tl = divide(fs(lo, k0), d);
        V4 diff = vfs(C1, C0);
        col[1] = vfmadd(diff, th, C0);
        col[0] = vfmadd(diff, tl, C0);
        last = 1;
    } else {
        float th = divide(fs(hi, k1), fs(k2, k1));
        float tl = divide(fs(lo, k0), fs(k1, k0));
        f[1] = divide(fs(k1, lo), fs(hi, lo));
        col[2] = vfmadd(vfs(C2, C1), th, C1);
        col[0] = vfmadd(vfs(C1, C0), tl, C0);
        col[1] = C1;
        last = 2;
    }
    f[last] = 1.0f;
    float hf = cuflt(height);
    uint32_t rows[8];
    for (int i = 0; i <= last; i++) rows[i] = cfltu(fm(hf, f[i]));
    table.assign(height, V4{0.0f, 0.0f, 0.0f, 0.0f});
    uint32_t out = 0;
    for (int i = 1; i <= last; i++) {
        if (!(rows[i] > rows[i - 1])) continue;
        uint32_t n = rows[i] - rows[i - 1];
        float nf = cuflt(n);
        float r = recip(nf);
        float t = fm(1.0f, r);
        float e = fnmsub(t, nf, 1.0f);
        float inv = fmadd(e, r, t);
        V4 step = vfms(vfs(col[i], col[i - 1]), inv);
        V4 c = col[i - 1];
        for (uint32_t k = 0; k < n && out < height; k++) {
            table[out++] = c;
            c = vfa(c, step);
        }
    }
}

void store_texel(uint8_t* p, uint16_t r, uint16_t g, uint16_t b, uint16_t a) {
    p[0] = uint8_t(r >> 8);
    p[1] = uint8_t(r);
    p[2] = uint8_t(g >> 8);
    p[3] = uint8_t(g);
    p[4] = uint8_t(b >> 8);
    p[5] = uint8_t(b);
    p[6] = uint8_t(a >> 8);
    p[7] = uint8_t(a);
}

struct SliceConsts {
    float hi, lo, half, mid, scale;
    float ow[4], oz[4];
};

struct Texel {
    float acc;
    uint16_t alpha;
};

Texel texel_scalar(float acc, float wx, float wy, float wz, const V4& cpos, const SliceConsts& k, float ra) {
    float s = acc;
    for (int i = 0; i < 4; i++) {
        float px = fmadd(cpos[0], k.ow[i], fm(wx, k.oz[i]));
        float py = fmadd(cpos[1], k.ow[i], fm(wy, k.oz[i]));
        float pzv = fmadd(cpos[2], k.ow[i], fm(wz, k.oz[i]));
        s = fa(s, noise3(px, py, pzv));
    }
    float v = fmadd(fm(s, k.scale), k.half, k.mid);
    if (gt(v, k.hi)) v = k.hi;
    if (!gt(v, k.lo)) v = k.lo;
    return {s, to_half(fm(ra, v))};
}

struct alignas(32) Tables {
    int32_t pair[512];
    Tables() {
        for (int i = 0; i < 512; i++) pair[i] = kPerm[i & 255] | (kPerm[(i + 1) & 255] << 16);
    }
};

const Tables g_tables;

struct Simd {
    __m256i spec = _mm256_setzero_si256();

    static __m256 pz(__m256 r) { return _mm256_andnot_ps(_mm256_cmp_ps(r, _mm256_setzero_ps(), _CMP_EQ_OQ), r); }
    __m256 chk(__m256 r) {
        __m256i mag = _mm256_and_si256(_mm256_castps_si256(r), _mm256_set1_epi32(0x7FFFFFFF));
        spec = _mm256_or_si256(spec, _mm256_cmpgt_epi32(mag, _mm256_set1_epi32(0x7F7FFFFE)));
        return pz(r);
    }
    __m256 fa(__m256 a, __m256 b) { return chk(_mm256_add_ps(a, b)); }
    __m256 fs(__m256 a, __m256 b) { return chk(_mm256_sub_ps(a, b)); }
    __m256 fm(__m256 a, __m256 b) { return chk(_mm256_mul_ps(a, b)); }
    __m256 fma(__m256 a, __m256 b, __m256 c) { return chk(_mm256_fmadd_ps(a, b, c)); }
    static __m256 ua(__m256 a, __m256 b) { return pz(_mm256_add_ps(a, b)); }
    static __m256 us(__m256 a, __m256 b) { return pz(_mm256_sub_ps(a, b)); }
    static __m256 um(__m256 a, __m256 b) { return pz(_mm256_mul_ps(a, b)); }
    static __m256 uma(__m256 a, __m256 b, __m256 c) { return pz(_mm256_fmadd_ps(a, b, c)); }
    static __m256 ums(__m256 a, __m256 b, __m256 c) { return pz(_mm256_fmsub_ps(a, b, c)); }
    static __m256 lerp(__m256 a, __m256 b, __m256 t) { return uma(us(b, a), t, a); }

    static __m256i key(__m256 a) {
        __m256i b = _mm256_castps_si256(a);
        __m256i mag = _mm256_and_si256(b, _mm256_set1_epi32(0x7FFFFFFF));
        __m256i zero = _mm256_cmpeq_epi32(_mm256_and_si256(b, _mm256_set1_epi32(0x7F800000)), _mm256_setzero_si256());
        mag = _mm256_andnot_si256(zero, mag);
        __m256i neg = _mm256_srai_epi32(b, 31);
        return _mm256_sub_epi32(_mm256_xor_si256(mag, neg), neg);
    }
    static __m256 gt_mask(__m256 a, __m256 b) { return _mm256_castsi256_ps(_mm256_cmpgt_epi32(key(a), key(b))); }

    static void perm2(__m256i i, __m256i& a, __m256i& b) {
        __m256i v = _mm256_i32gather_epi32(g_tables.pair, i, 4);
        a = _mm256_and_si256(v, _mm256_set1_epi32(0xFFFF));
        b = _mm256_srli_epi32(v, 16);
    }

    static __m256 grad(__m256i h, __m256 x, __m256 y, __m256 z) {
        const __m256i sign = _mm256_set1_epi32(int32_t(0x80000000u));
        __m256 u = _mm256_blendv_ps(x, y, _mm256_castsi256_ps(_mm256_slli_epi32(h, 28)));
        __m256i h12 = _mm256_and_si256(h, _mm256_set1_epi32(12));
        __m256 v = _mm256_blendv_ps(z, y, _mm256_castsi256_ps(_mm256_cmpeq_epi32(h12, _mm256_setzero_si256())));
        __m256i only_y = _mm256_cmpeq_epi32(_mm256_and_si256(h, _mm256_set1_epi32(13)), _mm256_set1_epi32(12));
        v = _mm256_andnot_ps(_mm256_castsi256_ps(only_y), v);
        u = _mm256_xor_ps(u, _mm256_castsi256_ps(_mm256_slli_epi32(h, 31)));
        v = _mm256_xor_ps(v, _mm256_castsi256_ps(_mm256_and_si256(_mm256_slli_epi32(h, 30), sign)));
        return ua(u, v);
    }

    void floor_frac(__m256 p, __m256i& cell, __m256& frac, __m256& fade) {
        __m256i pb = _mm256_castps_si256(p);
        __m256 s = _mm256_castsi256_ps(_mm256_and_si256(_mm256_srai_epi32(pb, 31), _mm256_set1_epi32(0x3F7FFFFF)));
        __m256i qb = _mm256_castps_si256(fs(p, s));
        __m256i e = _mm256_and_si256(_mm256_srli_epi32(qb, 23), _mm256_set1_epi32(0xFF));
        __m256i mask = _mm256_srlv_epi32(_mm256_set1_epi32(0x7FFFFF), _mm256_sub_epi32(e, _mm256_set1_epi32(127)));
        __m256i small = _mm256_cmpgt_epi32(_mm256_set1_epi32(127), e);
        __m256i tb = _mm256_andnot_si256(small, _mm256_andnot_si256(mask, qb));
        __m256 t = _mm256_castsi256_ps(tb);
        __m256i ci = _mm256_cvttps_epi32(t);
        __m256i big = _mm256_cmpgt_epi32(_mm256_and_si256(tb, _mm256_set1_epi32(0x7FFFFFFF)), _mm256_set1_epi32(0x4EFFFFFF));
        __m256i sat = _mm256_xor_si256(_mm256_set1_epi32(0x7FFFFFFF), _mm256_srai_epi32(tb, 31));
        ci = _mm256_blendv_epi8(ci, sat, big);
        cell = _mm256_and_si256(ci, _mm256_set1_epi32(255));
        frac = fs(p, t);
        __m256 f3 = um(um(frac, frac), frac);
        __m256 poly = uma(frac, ums(frac, _mm256_set1_ps(6.0f), _mm256_set1_ps(15.0f)), _mm256_set1_ps(10.0f));
        fade = um(f3, poly);
    }

    template <int N>
    void noise_n(const __m256* px, const __m256* py, const __m256* pzv, __m256* out) {
        __m256i cx[N], cy[N], cz[N];
        __m256 x0[N], y0[N], z0[N], ux[N], uy[N], uz[N];
        for (int n = 0; n < N; n++) {
            floor_frac(px[n], cx[n], x0[n], ux[n]);
            floor_frac(py[n], cy[n], y0[n], uy[n]);
            floor_frac(pzv[n], cz[n], z0[n], uz[n]);
        }
        const __m256i one = _mm256_set1_epi32(1);
        __m256i A[N], B[N];
        for (int n = 0; n < N; n++) {
            __m256i px0, px1;
            perm2(cx[n], px0, px1);
            A[n] = _mm256_add_epi32(px0, cy[n]);
            B[n] = _mm256_add_epi32(px1, cy[n]);
        }
        __m256i AA[N], AB[N], BA[N], BB[N];
        for (int n = 0; n < N; n++) {
            __m256i a0, a1, b0, b1;
            perm2(A[n], a0, a1);
            perm2(B[n], b0, b1);
            AA[n] = _mm256_add_epi32(a0, cz[n]);
            AB[n] = _mm256_add_epi32(a1, cz[n]);
            BA[n] = _mm256_add_epi32(b0, cz[n]);
            BB[n] = _mm256_add_epi32(b1, cz[n]);
        }
        const __m256 onef = _mm256_set1_ps(1.0f);
        for (int n = 0; n < N; n++) {
            __m256 x1 = us(x0[n], onef), y1 = us(y0[n], onef), z1 = us(z0[n], onef);
            __m256i hAA0, hAA1, hBA0, hBA1, hAB0, hAB1, hBB0, hBB1;
            perm2(AA[n], hAA0, hAA1);
            perm2(BA[n], hBA0, hBA1);
            perm2(AB[n], hAB0, hAB1);
            perm2(BB[n], hBB0, hBB1);
            __m256 x00 = lerp(grad(hAA0, x0[n], y0[n], z0[n]), grad(hBA0, x1, y0[n], z0[n]), ux[n]);
            __m256 x10 = lerp(grad(hAB0, x0[n], y1, z0[n]), grad(hBB0, x1, y1, z0[n]), ux[n]);
            __m256 x01 = lerp(grad(hAA1, x0[n], y0[n], z1), grad(hBA1, x1, y0[n], z1), ux[n]);
            __m256 x11 = lerp(grad(hAB1, x0[n], y1, z1), grad(hBB1, x1, y1, z1), ux[n]);
            out[n] = lerp(lerp(x00, x10, uy[n]), lerp(x01, x11, uy[n]), uz[n]);
        }
    }

    __m256i to_half(__m256 x) {
        __m256i b = _mm256_castps_si256(fm(x, _mm256_castsi256_ps(_mm256_set1_epi32(0x07800000))));
        __m256i h = _mm256_or_si256(_mm256_slli_epi32(b, 3), _mm256_and_si256(b, _mm256_set1_epi32(int32_t(0x80000000u))));
        return _mm256_srli_epi32(h, 16);
    }
};

void slice_row_simd(float* acc, const float* wx, const float* wy, const float* wz, uint32_t width, const V4& cpos, const SliceConsts& k, float ra, uint16_t* alpha) {
    uint32_t x = 0;
    const __m256 cx = _mm256_set1_ps(cpos[0]), cy = _mm256_set1_ps(cpos[1]), cz = _mm256_set1_ps(cpos[2]);
    const __m256 hi = _mm256_set1_ps(k.hi), lo = _mm256_set1_ps(k.lo), half = _mm256_set1_ps(k.half), mid = _mm256_set1_ps(k.mid), scale = _mm256_set1_ps(k.scale), rav = _mm256_set1_ps(ra);
    for (; x + 8 <= width; x += 8) {
        Simd m;
        __m256 a0 = _mm256_loadu_ps(acc + x);
        __m256 vx = _mm256_loadu_ps(wx + x), vy = _mm256_loadu_ps(wy + x), vz = _mm256_loadu_ps(wz + x);
        __m256 s = a0;
        __m256 px[4], py[4], pzv[4], n[4];
        for (int i = 0; i < 4; i++) {
            __m256 ow = _mm256_set1_ps(k.ow[i]), oz = _mm256_set1_ps(k.oz[i]);
            px[i] = m.fma(cx, ow, m.fm(vx, oz));
            py[i] = m.fma(cy, ow, m.fm(vy, oz));
            pzv[i] = m.fma(cz, ow, m.fm(vz, oz));
        }
        m.noise_n<4>(px, py, pzv, n);
        for (int i = 0; i < 4; i++) s = m.fa(s, n[i]);
        __m256 v = m.fma(m.fm(s, scale), half, mid);
        v = _mm256_blendv_ps(v, hi, Simd::gt_mask(v, hi));
        v = _mm256_blendv_ps(lo, v, Simd::gt_mask(v, lo));
        __m256i h = m.to_half(m.fm(rav, v));
        alignas(32) float sv[8];
        alignas(32) int32_t hv[8];
        _mm256_store_ps(sv, s);
        _mm256_store_si256(reinterpret_cast<__m256i*>(hv), h);
        int bad = _mm256_movemask_ps(_mm256_castsi256_ps(m.spec));
        for (int l = 0; l < 8; l++) {
            if (bad & (1 << l)) {
                Texel t = texel_scalar(acc[x + l], wx[x + l], wy[x + l], wz[x + l], cpos, k, ra);
                acc[x + l] = t.acc;
                alpha[x + l] = t.alpha;
            } else {
                acc[x + l] = sv[l];
                alpha[x + l] = uint16_t(hv[l]);
            }
        }
    }
    for (; x < width; x++) {
        Texel t = texel_scalar(acc[x], wx[x], wy[x], wz[x], cpos, k, ra);
        acc[x] = t.acc;
        alpha[x] = t.alpha;
    }
}

struct CsrGuard {
    unsigned old;
    CsrGuard() : old(_mm_getcsr()) { _mm_setcsr((old & ~0x6000u) | 0x6000u | 0x8040u); }
    ~CsrGuard() { _mm_setcsr(old); }
};

}

Job parse_args(const uint8_t* a) {
    Job j;
    j.out_ea = be32(a + 0x20);
    j.width = be32(a + 0x24);
    j.height = be32(a + 0x28);
    j.depth = be32(a + 0x2c);
    j.rows = be32(a + 0x30);
    j.row_start = be32(a + 0x34);
    j.param_ea = be32(a + 0x48);
    return j;
}

uint32_t output_size(const Job& job) { return job.width * job.rows * job.depth * 8; }

float perlin_noise(float x, float y, float z) {
    CsrGuard csr;
    return noise3(x, y, z);
}

void run_job(uint8_t* param_be, const Job& job, uint8_t* out_be) {
    CsrGuard csr;
    Params P;
    for (int i = 0; i < 80; i++) P.w[i] = fl(be32(param_be + i * 4));
    const uint32_t W = job.width, H = job.height, D = job.depth, R = job.rows;
    std::vector<V4> table;
    build_row_colours(P, param_be, H, table);
    auto row_colour = [&](uint32_t r) { return r < table.size() ? table[r] : V4{0.0f, 0.0f, 0.0f, 0.0f}; };

    float a = P.w[0x40 / 4], b = P.w[0x44 / 4];
    float hf = cuflt(H);
    float da = divide(fm(a, -2.0f), hf);
    float wh = fs(cuflt(W, -1), 0.5f);
    float db = divide(neg(b), wh);
    SinCos sa = spu_sincos(a), sb = spu_sincos(b), sda = spu_sincos(da), sdb = spu_sincos(db);

    const V4 e0{1.0f, 0.0f, 0.0f, 0.0f}, e1{0.0f, 1.0f, 0.0f, 0.0f}, e3{0.0f, 0.0f, 0.0f, 1.0f};
    M4 Rb = {V4{sb.c, 0.0f, neg(sb.s), 0.0f}, e1, V4{sb.s, 0.0f, sb.c, 0.0f}, e3};
    M4 M = {e0, V4{0.0f, sa.c, sa.s, 0.0f}, V4{0.0f, neg(sa.s), sa.c, 0.0f}, e3};
    left_mul(Rb, M);
    M4 Astep = {e0, V4{0.0f, sda.c, sda.s, 0.0f}, V4{0.0f, neg(sda.s), sda.c, 0.0f}, e3};
    M4 Bstep = {V4{sdb.c, 0.0f, neg(sdb.s), 0.0f}, e1, V4{sdb.s, 0.0f, sdb.c, 0.0f}, e3};
    for (uint32_t i = 0; i < job.row_start; i++) left_mul(Astep, M);

    V4 cpos = vfa(P.vec(0x30), P.vec(0x50));
    V4 cam[4] = {P.vec(0x00), P.vec(0x10), P.vec(0x20), cpos};

    std::vector<V4> w2(size_t(R) * W);
    {
        M4 Mr = {M[0], M[1], M[2], M[3]};
        for (uint32_t r = 0; r < R; r++) {
            M4 T = {Mr[0], Mr[1], Mr[2], Mr[3]};
            for (uint32_t x = 0; x < W; x++) {
                w2[size_t(r) * W + x] = combine(cam, T[2]);
                right_mul(T, Bstep);
            }
            left_mul(Astep, Mr);
        }
    }

    const uint32_t slice_bytes = R * W * 8;
    V4 mult = P.vec(0x120);
    for (uint32_t r = 0; r < R; r++) {
        V4 c = vfm(row_colour(r), mult);
        uint16_t hr = to_half(clamp01(c[0])), hg = to_half(clamp01(c[1])), hb = to_half(clamp01(c[2]));
        for (uint32_t x = 0; x < W; x++) store_texel(out_be + size_t(r) * W * 8 + x * 8, hr, hg, hb, 0);
    }
    if (D <= 1) return;

    float dm1 = cuflt(D - 1);
    float rd = recip(dm1);
    float inv_dm1 = fmadd(fnmsub(rd, dm1, 1.0f), rd, rd);
    V4 mstep = {fmadd(inv_dm1, fs(P.w[0x130 / 4], P.w[0x120 / 4]), 0.0f), fmadd(inv_dm1, fs(P.w[0x134 / 4], P.w[0x124 / 4]), 0.0f),
                fmadd(inv_dm1, fs(P.w[0x138 / 4], P.w[0x128 / 4]), 0.0f), fmadd(inv_dm1, fs(P.w[0x13c / 4], P.w[0x12c / 4]), 0.0f)};

    V4 o[4];
    o[0] = V4{0.0f, 0.0f, neg(P.w[0x70 / 4]), P.w[0x78 / 4]};
    V4 q{0.0f, 0.0f, fm(P.w[0x74 / 4], -0.25f), 0.0f};
    for (int k = 1; k < 4; k++) o[k] = vfa(o[k - 1], q);
    V4 dz{0.0f, 0.0f, neg(P.w[0x74 / 4]), 0.0f};

    std::vector<float> acc(size_t(R) * W, 0.0f), wx(size_t(R) * W), wy(size_t(R) * W), wz(size_t(R) * W);
    for (size_t i = 0; i < w2.size(); i++) {
        wx[i] = w2[i][0];
        wy[i] = w2[i][1];
        wz[i] = w2[i][2];
    }
    std::vector<uint16_t> alpha(W);
    for (uint32_t z = 1; z < D; z++) {
        SliceConsts k;
        float zf = cuflt(z);
        float rz = recip(dm1);
        float t0 = fm(zf, rz);
        float t = fmadd(fnmsub(t0, dm1, zf), rz, t0);
        k.hi = fmadd(t, fs(P.w[0x88 / 4], P.w[0x80 / 4]), P.w[0x80 / 4]);
        k.lo = fmadd(t, fs(P.w[0x84 / 4], P.w[0x7c / 4]), P.w[0x7c / 4]);
        k.half = fm(fs(k.hi, k.lo), 0.5f);
        k.mid = fa(k.half, k.lo);
        float z1 = fa(zf, 1.0f);
        float rz1 = recip(z1);
        float s0 = fm(P.w[0x8c / 4], rz1);
        k.scale = fmadd(fnmsub(s0, z1, P.w[0x8c / 4]), rz1, s0);
        mult = vfa(mult, mstep);
        for (int i = 0; i < 4; i++) {
            o[i] = vfa(o[i], dz);
            k.ow[i] = o[i][3];
            k.oz[i] = o[i][2];
        }
        uint8_t* slice = out_be + size_t(z) * slice_bytes;
        for (uint32_t r = 0; r < R; r++) {
            V4 c = vfm(row_colour(job.row_start + r), mult);
            uint16_t hr = to_half(clamp01(c[0])), hg = to_half(clamp01(c[1])), hb = to_half(clamp01(c[2]));
            size_t base = size_t(r) * W;
            slice_row_simd(acc.data() + base, wx.data() + base, wy.data() + base, wz.data() + base, W, cpos, k, c[3], alpha.data());
            for (uint32_t x = 0; x < W; x++) store_texel(slice + (base + x) * 8, hr, hg, hb, alpha[x]);
        }
    }
}

void run_volume(uint8_t* param_be, uint32_t width, uint32_t height, uint32_t depth, uint32_t rows_per_job, uint8_t* volume_be) {
    Job job;
    job.width = width;
    job.height = height;
    job.depth = depth;
    job.rows = rows_per_job;
    std::vector<uint8_t> tmp(output_size(job));
    for (uint32_t r0 = 0; r0 < height; r0 += rows_per_job) {
        job.row_start = r0;
        run_job(param_be, job, tmp.data());
        for (uint32_t z = 0; z < depth; z++)
            std::memcpy(volume_be + (size_t(z) * height + r0) * width * 8, tmp.data() + size_t(z) * rows_per_job * width * 8, size_t(rows_per_job) * width * 8);
    }
}

}
