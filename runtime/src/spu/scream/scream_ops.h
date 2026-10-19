#pragma once

#include "../renjob/renjob_ops.h"

#include <bit>
#include <cstdint>

namespace renjob::xo {

struct DRound {
    unsigned old;
    DRound() : old(_mm_getcsr()) { _mm_setcsr((old & ~0x6000u) & ~0x8040u); }
    ~DRound() { _mm_setcsr(old); }
};

template <class Fn>
inline V dmap(V x, V y, Fn f) {
    DRound g;
    alignas(16) uint64_t a[2], b[2], r[2];
    _mm_store_si128(reinterpret_cast<__m128i*>(a), x);
    _mm_store_si128(reinterpret_cast<__m128i*>(b), y);
    for (int i = 0; i < 2; i++) r[i] = std::bit_cast<uint64_t>(f(std::bit_cast<double>(a[i]), std::bit_cast<double>(b[i])));
    return _mm_load_si128(reinterpret_cast<const __m128i*>(r));
}

inline V dfa(V x, V y) { return dmap(x, y, [](double p, double q) { return p + q; }); }
inline V dfs(V x, V y) { return dmap(x, y, [](double p, double q) { return p - q; }); }
inline V dfm(V x, V y) { return dmap(x, y, [](double p, double q) { return p * q; }); }

inline V fesd(V x) {
    DRound g;
    alignas(16) uint32_t a[4];
    alignas(16) uint64_t r[2];
    _mm_store_si128(reinterpret_cast<__m128i*>(a), x);
    r[1] = std::bit_cast<uint64_t>(double(std::bit_cast<float>(a[3])));
    r[0] = std::bit_cast<uint64_t>(double(std::bit_cast<float>(a[1])));
    return _mm_load_si128(reinterpret_cast<const __m128i*>(r));
}

inline V frds(V x) {
    DRound g;
    alignas(16) uint64_t a[2];
    alignas(16) uint32_t r[4] = {};
    _mm_store_si128(reinterpret_cast<__m128i*>(a), x);
    r[3] = std::bit_cast<uint32_t>(float(std::bit_cast<double>(a[1])));
    r[1] = std::bit_cast<uint32_t>(float(std::bit_cast<double>(a[0])));
    return _mm_load_si128(reinterpret_cast<const __m128i*>(r));
}

}
