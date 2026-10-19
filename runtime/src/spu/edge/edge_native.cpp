#include "edge_native.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace edge {

namespace {

inline uint32_t be16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }
inline uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
inline void put_be16(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}
inline void put_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

constexpr uint32_t kLsMask = 0x3ffff;

}

template <bool Wrap>
struct Mem {
    uint8_t* ls;
    inline uint32_t m(uint32_t a) const { return Wrap ? (a & kLsMask) : a; }
    inline uint32_t r8(uint32_t a) const { return ls[m(a)]; }
    inline uint32_t r16(uint32_t a) const {
        if constexpr (Wrap) return (r8(a) << 8) | r8(a + 1);
        uint16_t v;
        std::memcpy(&v, ls + a, 2);
        return __builtin_bswap16(v);
    }
    inline uint32_t r32(uint32_t a) const {
        if constexpr (Wrap) return (r8(a) << 24) | (r8(a + 1) << 16) | (r8(a + 2) << 8) | r8(a + 3);
        uint32_t v;
        std::memcpy(&v, ls + a, 4);
        return __builtin_bswap32(v);
    }
    inline void w8(uint32_t a, uint32_t v) const { ls[m(a)] = uint8_t(v); }
    inline void w16(uint32_t a, uint32_t v) const {
        if constexpr (Wrap) {
            w8(a, v >> 8);
            w8(a + 1, v);
        } else {
            uint16_t x = __builtin_bswap16(uint16_t(v));
            std::memcpy(ls + a, &x, 2);
        }
    }
};

template <bool Wrap>
uint32_t decompress_indexes_impl(uint8_t* ls, uint32_t ctx, uint32_t src, Outputs* outs) {
    Mem<Wrap> M{ls};
    uint32_t num_indexes = M.r16(ctx + 400);
    uint32_t ntri = (num_indexes * 0xaaabu) & 0xffff;
    uint32_t n_explicit = M.r16(src + 0);
    uint32_t bias = M.r16(src + 2);
    uint32_t flag_bytes = M.r16(src + 4);
    uint32_t width = M.r8(src + 6);
    uint32_t flags = src + 8;
    uint32_t codes = flags + flag_bytes;
    uint32_t code_bytes = (2 * ntri + 7) >> 3;
    uint32_t packed = codes + code_bytes;
    uint32_t ubase = src + ((8 + flag_bytes + code_bytes + 15) & ~15u);
    uint32_t groups = (n_explicit + 31) >> 5;
    if (width == 0) {
        for (uint32_t q = 0; q < ((n_explicit + 7) >> 3); q++)
            for (uint32_t b = 0; b < 16; b++) M.w8(ubase + q * 16 + b, 0);
    } else {
        uint32_t mask = (1u << width) - 1;
        for (uint32_t g = groups; g-- > 0;) {
            uint16_t vals[32];
            uint32_t bit = g * 32 * width;
            for (uint32_t k = 0; k < 32; k++, bit += width) {
                uint32_t win = M.r32(packed + (bit >> 3));
                vals[k] = uint16_t((win >> (32 - width - (bit & 7))) & mask);
            }
            for (uint32_t k = 0; k < 32; k++) M.w16(ubase + 64 * g + 2 * k, vals[k]);
        }
    }
    if (n_explicit) {
        uint16_t carry[8] = {};
        for (uint32_t i = 0; i < groups * 32; i++) {
            uint16_t v = uint16_t(M.r16(ubase + 2 * i) - bias + carry[i & 7]);
            carry[i & 7] = v;
            M.w16(ubase + 2 * i, v);
        }
    }
    uint32_t sbase = src + ((6 * ntri + 15) & ~15u);
    uint32_t sgroups = (8 * flag_bytes + 15) >> 4;
    uint32_t nS = sgroups * 16;
    bool s_flat = sgroups && (sbase >= ubase + 64 * groups + 64 || sbase + 32 * sgroups <= ubase);
    if (s_flat) {
        uint32_t seq = 0, e = ubase, out = sbase;
        for (uint32_t k = 0; k < sgroups; k++) {
            uint32_t fb = (M.r8(flags + 2 * k) << 8) | M.r8(flags + 2 * k + 1);
            for (uint32_t i = 0; i < 16; i++) {
                uint32_t bit = (fb >> (15 - i)) & 1;
                uint32_t ev = M.r16(e);
                M.w16(out, bit ? ev : seq);
                out += 2;
                e += 2 * bit;
                seq += 1 - bit;
            }
        }
    } else {
        uint32_t seq = 0, ex = ubase;
        uint32_t fb = 0;
        uint32_t vals[16];
        for (uint32_t k = 0; k <= sgroups; k++) {
            uint32_t nb = (M.r8(flags + 2 * k) << 8) | M.r8(flags + 2 * k + 1);
            if (k == 0) {
                uint32_t v = M.r16(ubase);
                for (uint32_t i = 0; i < 16; i++) vals[i] = v;
            } else {
                uint32_t e = ex, s0 = seq;
                for (uint32_t i = 0; i < 16; i++) {
                    uint32_t bit = (fb >> (15 - i)) & 1;
                    uint32_t ev = M.r16(e);
                    vals[i] = bit ? ev : (s0 & 0xffff);
                    e += 2 * bit;
                    s0 += 1 - bit;
                }
                ex = e;
                seq = s0;
            }
            uint32_t at = sbase + 32 * (k ? k - 1 : 0);
            for (uint32_t i = 0; i < 16; i++) M.w16(at + 2 * i, vals[i]);
            fb = nb;
        }
    }
    uint32_t cbase = sbase + 2 * nS;
    {
        uint32_t k16 = (code_bytes + 15) >> 4;
        uint8_t prev[16] = {};
        for (uint32_t k = 0; k <= k16; k++) {
            uint32_t at = cbase + 16 * (k ? k - 1 : 0);
            for (uint32_t b = 0; b < 16; b++) M.w8(at + b, prev[b]);
            for (uint32_t b = 0; b < 16; b++) prev[b] = uint8_t(M.r8(codes + 16 * k + b));
        }
    }
    uint32_t tgroups = (ntri + 7) >> 3;
    bool t_flat = tgroups && src + 48 * tgroups <= sbase && src + 48 * tgroups <= cbase;
    if (t_flat) {
        uint32_t a = 0, b = 0, c = 0, sp = sbase, out = src;
        for (uint32_t g = 0; g < tgroups; g++) {
            uint32_t cw = M.r16(cbase + 2 * g);
            for (uint32_t t = 0; t < 8; t++) {
                uint32_t code = (cw >> (14 - 2 * t)) & 3;
                uint32_t s0 = M.r16(sp), s1 = M.r16(sp + 2), s2 = M.r16(sp + 4);
                uint32_t m0 = 0u - (code == 0), m1 = 0u - (code == 1), m2 = 0u - (code == 2), m3 = 0u - (code == 3);
                uint32_t x = (a & m0) | (c & m1) | (b & m2) | (s0 & m3);
                uint32_t y = (c & m0) | (b & m1) | (a & m2) | (s1 & m3);
                uint32_t z = (s2 & m3) | (s0 & ~m3);
                sp += 2 + (m3 & 4);
                M.w16(out, x);
                M.w16(out + 2, y);
                M.w16(out + 4, z);
                out += 6;
                a = x;
                b = y;
                c = z;
            }
        }
    } else {
        uint32_t a = 0, b = 0, c = 0;
        uint32_t sp = sbase;
        uint32_t tri[24] = {};
        for (uint32_t g = 0; g <= tgroups; g++) {
            uint32_t cur[24];
            if (g < tgroups) {
                uint32_t cw = M.r16(cbase + 2 * g);
                for (uint32_t t = 0; t < 8; t++) {
                    uint32_t code = (cw >> (14 - 2 * t)) & 3;
                    uint32_t s0 = M.r16(sp), s1 = M.r16(sp + 2), s2 = M.r16(sp + 4);
                    uint32_t m0 = 0u - (code == 0), m1 = 0u - (code == 1), m2 = 0u - (code == 2), m3 = 0u - (code == 3);
                    uint32_t x = (a & m0) | (c & m1) | (b & m2) | (s0 & m3);
                    uint32_t y = (c & m0) | (b & m1) | (a & m2) | (s1 & m3);
                    uint32_t z = (s2 & m3) | (s0 & ~m3);
                    sp += 2 + (m3 & 4);
                    cur[t * 3 + 0] = x;
                    cur[t * 3 + 1] = y;
                    cur[t * 3 + 2] = z;
                    a = x;
                    b = y;
                    c = z;
                }
            }
            uint32_t at = src + 48 * (g ? g - 1 : 0);
            for (uint32_t k = 0; k < 24; k++) M.w16(at + 2 * k, tri[k]);
            if (g < tgroups) std::memcpy(tri, cur, sizeof tri);
        }
    }
    uint32_t end = (src + 48 * tgroups + 15) & ~15u;
    M.w16(ctx + 88, end >> 16);
    M.w16(ctx + 90, end);
    if (outs) {
        outs->add(src, src + ntri * 6);
        outs->add(ctx + 88, ctx + 92);
    }
    return end;
}

bool decompress_indexes_supported(const uint8_t* ls, uint32_t src) {
    uint32_t n = be16(ls + (src & kLsMask));
    uint32_t w = ls[(src + 6) & kLsMask];
    return w < 16 && (n != 0 || w == 0) && (src & 15) == 0;
}

uint32_t decompress_indexes(uint8_t* ls, uint32_t ctx, uint32_t src, Outputs* outs) {
    static const bool no_fast = std::getenv("EDGE_IDX_SLOW") != nullptr;
    uint32_t fend = 0;
    if (!no_fast && decompress_indexes_fast(ls, ctx, src, &fend)) {
        if (outs) {
            uint32_t nt = (be16(ls + ctx + 400) * 0xaaabu) & 0xffff;
            outs->add(src, src + nt * 6);
            outs->add(ctx + 88, ctx + 92);
        }
        return fend;
    }
    uint32_t num_indexes = be16(ls + ((ctx + 400) & kLsMask));
    uint32_t ntri = (num_indexes * 0xaaabu) & 0xffff;
    uint64_t n_explicit = be16(ls + (src & kLsMask)), flag_bytes = be16(ls + ((src + 4) & kLsMask));
    uint64_t code_bytes = (2 * ntri + 7) >> 3;
    uint64_t sgroups = (8 * flag_bytes + 15) >> 4;
    uint64_t span = 64 + 8 + flag_bytes + code_bytes + 4 * 16 * ((n_explicit + 31) >> 5) + 64 * ((n_explicit + 31) >> 5) + 96 * ((ntri + 7) >> 3) + 64 * (sgroups + 1) + 2 * (code_bytes + 32) + 6 * 16 * (sgroups + 1);
    if (src + span + 64 < 0x40000 && ctx + 512 < 0x40000) return decompress_indexes_impl<false>(ls, ctx, src, outs);
    return decompress_indexes_impl<true>(ls, ctx, src, outs);
}

}
