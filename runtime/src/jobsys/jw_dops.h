#pragma once

#include "renjob_ops.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <immintrin.h>

namespace renjob::xo {

struct DRound {
    unsigned old;
    DRound() : old(_mm_getcsr()) { _mm_setcsr((old & ~0x6000u) & ~0x8040u); }
    ~DRound() { _mm_setcsr(old); }
};

inline V d_pack(const uint64_t (&u)[2]) {
    V r;
    std::memcpy(&r, u, 16);
    return r;
}

template <class F>
inline V d_map2(V a, V b, F f) {
    DRound g;
    uint64_t x[2], y[2];
    std::memcpy(x, &a, 16);
    std::memcpy(y, &b, 16);
    for (int i = 0; i < 2; i++) x[i] = std::bit_cast<uint64_t>(f(std::bit_cast<double>(x[i]), std::bit_cast<double>(y[i])));
    return d_pack(x);
}

inline V dfa(V a, V b) { return d_map2(a, b, [](double x, double y) { return x + y; }); }
inline V dfs(V a, V b) { return d_map2(a, b, [](double x, double y) { return x - y; }); }
inline V dfm(V a, V b) { return d_map2(a, b, [](double x, double y) { return x * y; }); }

inline V d_fma(V a, V b, V t, int kind) {
    DRound g;
    uint64_t x[2], y[2], z[2], r[2];
    std::memcpy(x, &a, 16);
    std::memcpy(y, &b, 16);
    std::memcpy(z, &t, 16);
    for (int i = 0; i < 2; i++) {
        double p = std::bit_cast<double>(x[i]), q = std::bit_cast<double>(y[i]), s = std::bit_cast<double>(z[i]);
        double v = kind == 0 ? std::fma(p, q, s) : kind == 1 ? std::fma(p, q, -s) : kind == 2 ? -std::fma(p, q, -s) : -std::fma(p, q, s);
        r[i] = std::bit_cast<uint64_t>(v);
    }
    return d_pack(r);
}

inline V dfma(V a, V b, V t) { return d_fma(a, b, t, 0); }
inline V dfms(V a, V b, V t) { return d_fma(a, b, t, 1); }
inline V dfnms(V a, V b, V t) { return d_fma(a, b, t, 2); }
inline V dfnma(V a, V b, V t) { return d_fma(a, b, t, 3); }

inline V d_cmp(V a, V b, int kind) {
    uint64_t x[2], y[2], r[2];
    std::memcpy(x, &a, 16);
    std::memcpy(y, &b, 16);
    for (int i = 0; i < 2; i++) {
        double p = std::bit_cast<double>(x[i]), q = std::bit_cast<double>(y[i]);
        if (kind & 2) {
            p = std::fabs(p);
            q = std::fabs(q);
        }
        bool v = (kind & 1) ? p == q : p > q;
        r[i] = v ? ~0ull : 0;
    }
    return d_pack(r);
}

inline V dfcgt(V a, V b) { return d_cmp(a, b, 0); }
inline V dfceq(V a, V b) { return d_cmp(a, b, 1); }
inline V dfcmgt(V a, V b) { return d_cmp(a, b, 2); }
inline V dfcmeq(V a, V b) { return d_cmp(a, b, 3); }

inline V fesd(V a) {
    DRound g;
    float f[4];
    std::memcpy(f, &a, 16);
    uint64_t r[2];
    r[1] = std::bit_cast<uint64_t>(double(f[3]));
    r[0] = std::bit_cast<uint64_t>(double(f[1]));
    return d_pack(r);
}

inline V frds(V a) {
    DRound g;
    uint64_t d[2];
    std::memcpy(d, &a, 16);
    float f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    f[3] = float(std::bit_cast<double>(d[1]));
    f[1] = float(std::bit_cast<double>(d[0]));
    V r;
    std::memcpy(&r, f, 16);
    return r;
}

}
