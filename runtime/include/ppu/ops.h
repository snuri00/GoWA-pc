#pragma once

#include "context.h"

#include <atomic>
#include <bit>
#include <cmath>
#include <cfenv>
#include <limits>

namespace ppu {

void syscall(Context& ctx, uint8_t* base);
void call_import(Context& ctx, uint8_t* base, uint32_t index);
void call_indirect(Context& ctx, uint8_t* base, uint32_t addr);
void trap(Context& ctx, uint8_t* base, uint32_t pc);
void unimplemented(Context& ctx, uint8_t* base, uint32_t pc, uint32_t word);
uint64_t timebase();
uint32_t lwarx(Context& ctx, uint8_t* base, uint32_t ea);
uint64_t ldarx(Context& ctx, uint8_t* base, uint32_t ea);
bool stwcx(Context& ctx, uint8_t* base, uint32_t ea, uint32_t value);
bool stdcx(Context& ctx, uint8_t* base, uint32_t ea, uint64_t value);
void notify_store(uint8_t* base, uint32_t ea, uint32_t size);
double fres(double x);
double frsqrte(double x);
float vrefp(float x);
float vrsqrtefp(float x);

inline uint8_t ld8(uint8_t* b, uint32_t ea) { return b[ea]; }
inline uint16_t ld16(uint8_t* b, uint32_t ea) { uint16_t v; std::memcpy(&v, b + ea, 2); return __builtin_bswap16(v); }
inline uint32_t ld32(uint8_t* b, uint32_t ea) { uint32_t v; std::memcpy(&v, b + ea, 4); return __builtin_bswap32(v); }
inline uint64_t ld64(uint8_t* b, uint32_t ea) { uint64_t v; std::memcpy(&v, b + ea, 8); return __builtin_bswap64(v); }
inline void st8(uint8_t* b, uint32_t ea, uint8_t v) { b[ea] = v; }
inline void st16(uint8_t* b, uint32_t ea, uint16_t v) { v = __builtin_bswap16(v); std::memcpy(b + ea, &v, 2); }
inline void st32(uint8_t* b, uint32_t ea, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(b + ea, &v, 4); }
inline void st64(uint8_t* b, uint32_t ea, uint64_t v) { v = __builtin_bswap64(v); std::memcpy(b + ea, &v, 8); }

inline double ldf32(uint8_t* b, uint32_t ea) { return static_cast<double>(std::bit_cast<float>(ld32(b, ea))); }
inline double ldf64(uint8_t* b, uint32_t ea) { return std::bit_cast<double>(ld64(b, ea)); }
inline void stf32(uint8_t* b, uint32_t ea, double v) { st32(b, ea, std::bit_cast<uint32_t>(static_cast<float>(v))); }
inline void stf64(uint8_t* b, uint32_t ea, double v) { st64(b, ea, std::bit_cast<uint64_t>(v)); }

inline __m128i bswap128(__m128i x) { return _mm_shuffle_epi8(x, _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15)); }
inline v128 ldv(uint8_t* b, uint32_t ea) { v128 r; r.vi = bswap128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(b + (ea & ~15u)))); return r; }
inline void stv(uint8_t* b, uint32_t ea, const v128& v) { _mm_storeu_si128(reinterpret_cast<__m128i*>(b + (ea & ~15u)), bswap128(v.vi)); }

inline uint64_t rotl64(uint64_t x, uint32_t n) { return std::rotl(x, static_cast<int>(n & 63)); }
inline uint64_t dup32(uint64_t x) { uint64_t l = static_cast<uint32_t>(x); return l | (l << 32); }
inline uint64_t rotl32(uint64_t x, uint32_t n) { return rotl64(dup32(x), n & 31); }
constexpr uint64_t mask64(uint32_t mb, uint32_t me) {
    uint64_t m1 = ~0ull >> mb;
    uint64_t m2 = ~0ull << (63 - me);
    return mb <= me ? (m1 & m2) : (m1 | m2);
}

inline void cr_set(Context& c, int f, int64_t a, int64_t b) { c.cr[f] = {uint8_t(a < b), uint8_t(a > b), uint8_t(a == b), c.xer_so}; }
inline void cr_setu(Context& c, int f, uint64_t a, uint64_t b) { c.cr[f] = {uint8_t(a < b), uint8_t(a > b), uint8_t(a == b), c.xer_so}; }
inline void cr0(Context& c, uint64_t v) { cr_set(c, 0, static_cast<int64_t>(v), 0); }
inline bool crbit(const Context& c, uint32_t bi) { const CRField& f = c.cr[bi >> 2]; switch (bi & 3) { case 0: return f.lt; case 1: return f.gt; case 2: return f.eq; default: return f.so; } }
inline void crbit_set(Context& c, uint32_t bi, bool v) { CRField& f = c.cr[bi >> 2]; switch (bi & 3) { case 0: f.lt = v; break; case 1: f.gt = v; break; case 2: f.eq = v; break; default: f.so = v; break; } }
inline uint32_t cr_get(const Context& c) {
    uint32_t r = 0;
    for (int i = 0; i < 8; i++) r |= (uint32_t(c.cr[i].lt) << 3 | uint32_t(c.cr[i].gt) << 2 | uint32_t(c.cr[i].eq) << 1 | uint32_t(c.cr[i].so)) << (28 - 4 * i);
    return r;
}
inline void cr_put(Context& c, uint32_t mask8, uint32_t v) {
    for (int i = 0; i < 8; i++) {
        if (!(mask8 & (0x80u >> i))) continue;
        uint32_t n = (v >> (28 - 4 * i)) & 15;
        c.cr[i] = {uint8_t(n >> 3 & 1), uint8_t(n >> 2 & 1), uint8_t(n >> 1 & 1), uint8_t(n & 1)};
    }
}
inline uint64_t xer_get(const Context& c) { return uint64_t(c.xer_so) << 31 | uint64_t(c.xer_ov) << 30 | uint64_t(c.xer_ca) << 29 | c.xer_cnt; }
inline void xer_put(Context& c, uint64_t v) { c.xer_so = v >> 31 & 1; c.xer_ov = v >> 30 & 1; c.xer_ca = v >> 29 & 1; c.xer_cnt = v & 0x7f; }

inline bool bc_cond(Context& c, uint32_t bo, uint32_t bi) {
    if (!(bo & 4)) c.ctr--;
    bool ctr_ok = (bo & 4) || ((c.ctr != 0) ^ ((bo >> 1) & 1));
    bool cond_ok = (bo & 16) || (crbit(c, bi) == ((bo >> 3) & 1));
    return ctr_ok && cond_ok;
}

inline uint64_t add_ca(uint64_t a, uint64_t b, uint64_t ci, uint8_t& co) {
    uint64_t r = a + b;
    uint8_t c1 = r < a;
    uint64_t r2 = r + ci;
    co = c1 | (r2 < r);
    return r2;
}
inline void set_ov(Context& c, bool ov) { c.xer_ov = ov; c.xer_so |= ov; }
inline bool add_ov(uint64_t a, uint64_t b, uint64_t r) { return ((a ^ r) & (b ^ r)) >> 63; }

inline uint64_t slw(uint64_t s, uint64_t b) { uint32_t n = b & 63; return n > 31 ? 0 : static_cast<uint32_t>(static_cast<uint32_t>(s) << n); }
inline uint64_t srw(uint64_t s, uint64_t b) { uint32_t n = b & 63; return n > 31 ? 0 : static_cast<uint32_t>(s) >> n; }
inline uint64_t sld(uint64_t s, uint64_t b) { uint32_t n = b & 127; return n > 63 ? 0 : s << n; }
inline uint64_t srd(uint64_t s, uint64_t b) { uint32_t n = b & 127; return n > 63 ? 0 : s >> n; }
inline uint64_t sraw(Context& c, uint64_t s, uint64_t b) {
    int32_t v = static_cast<int32_t>(s);
    uint32_t n = b & 63;
    if (n > 31) { c.xer_ca = v < 0; return static_cast<uint64_t>(static_cast<int64_t>(v < 0 ? -1 : 0)); }
    int32_t r = v >> n;
    c.xer_ca = v < 0 && n && (static_cast<uint32_t>(v) & ((1u << n) - 1));
    return static_cast<uint64_t>(static_cast<int64_t>(r));
}
inline uint64_t srad(Context& c, uint64_t s, uint64_t b) {
    int64_t v = static_cast<int64_t>(s);
    uint32_t n = b & 127;
    if (n > 63) { c.xer_ca = v < 0; return static_cast<uint64_t>(v < 0 ? -1 : 0); }
    int64_t r = v >> n;
    c.xer_ca = v < 0 && n && (static_cast<uint64_t>(v) & ((1ull << n) - 1));
    return static_cast<uint64_t>(r);
}

inline uint64_t divw(uint64_t a, uint64_t b) { int32_t x = int32_t(a), y = int32_t(b); return (y == 0 || (x == INT32_MIN && y == -1)) ? 0 : uint32_t(x / y); }
inline uint64_t divwu(uint64_t a, uint64_t b) { uint32_t x = uint32_t(a), y = uint32_t(b); return y == 0 ? 0 : x / y; }
inline uint64_t divd(uint64_t a, uint64_t b) { int64_t x = int64_t(a), y = int64_t(b); return (y == 0 || (x == INT64_MIN && y == -1)) ? 0 : uint64_t(x / y); }
inline uint64_t divdu(uint64_t a, uint64_t b) { return b == 0 ? 0 : a / b; }
inline uint64_t mulhw(uint64_t a, uint64_t b) { return uint64_t(int64_t(int32_t(a)) * int64_t(int32_t(b)) >> 32); }
inline uint64_t mulhwu(uint64_t a, uint64_t b) { return (uint64_t(uint32_t(a)) * uint32_t(b)) >> 32; }
inline uint64_t mulhd(uint64_t a, uint64_t b) { return uint64_t((__int128)int64_t(a) * int64_t(b) >> 64); }
inline uint64_t mulhdu(uint64_t a, uint64_t b) { return uint64_t((unsigned __int128)a * b >> 64); }

inline double frsp(double x) { return static_cast<double>(static_cast<float>(x)); }
inline uint64_t f2u(double x) { return std::bit_cast<uint64_t>(x); }
inline double u2f(uint64_t x) { return std::bit_cast<double>(x); }
inline double fctiw(double x, bool trunc) {
    int32_t r;
    if (std::isnan(x)) r = INT32_MIN;
    else {
        double t = trunc ? std::trunc(x) : std::nearbyint(x);
        r = t >= 2147483647.0 ? INT32_MAX : t <= -2147483648.0 ? INT32_MIN : static_cast<int32_t>(t);
    }
    return u2f(0xFFF8000000000000ull | static_cast<uint32_t>(r));
}
inline double fctid(double x, bool trunc) {
    int64_t r;
    if (std::isnan(x)) r = INT64_MIN;
    else {
        double t = trunc ? std::trunc(x) : std::nearbyint(x);
        r = t >= 9223372036854775807.0 ? INT64_MAX : t <= -9223372036854775808.0 ? INT64_MIN : static_cast<int64_t>(t);
    }
    return u2f(static_cast<uint64_t>(r));
}
inline double fcfid(double x) { return static_cast<double>(static_cast<int64_t>(f2u(x))); }
inline void fcmp(Context& c, int f, double a, double b) {
    bool un = std::isnan(a) || std::isnan(b);
    c.cr[f] = {uint8_t(!un && a < b), uint8_t(!un && a > b), uint8_t(!un && a == b), uint8_t(un)};
    c.fpscr = (c.fpscr & ~0xF000u) | (uint32_t(c.cr[f].lt) << 15 | uint32_t(c.cr[f].gt) << 14 | uint32_t(c.cr[f].eq) << 13 | uint32_t(c.cr[f].so) << 12);
}
inline double fsel(double a, double b, double c) { return a >= 0.0 ? b : c; }
inline double fmadds(double a, double c, double b) { return frsp(std::fma(a, c, b)); }

inline __m128 vflush(__m128 x) {
    __m128i e = _mm_and_si128(_mm_castps_si128(x), _mm_set1_epi32(0x7F800000));
    __m128i z = _mm_cmpeq_epi32(e, _mm_setzero_si128());
    return _mm_castsi128_ps(_mm_andnot_si128(_mm_andnot_si128(_mm_set1_epi32(0x80000000), z), _mm_castps_si128(x)));
}
inline __m128 vnan(__m128 x) {
    __m128 n = _mm_cmpunord_ps(x, x);
    return _mm_or_ps(_mm_andnot_ps(n, x), _mm_and_ps(n, _mm_castsi128_ps(_mm_set1_epi32(0x7FC00000))));
}
inline __m128 vfix(const Context& c, __m128 x) { return vnan(c.vscr_nj ? vflush(x) : x); }
inline __m128 vin(const Context& c, __m128 x) { return c.vscr_nj ? vflush(x) : x; }

inline __m128i vperm(__m128i a, __m128i b, __m128i c) {
    __m128i idx = _mm_andnot_si128(c, _mm_set1_epi8(15));
    __m128i ra = _mm_shuffle_epi8(a, idx);
    __m128i rb = _mm_shuffle_epi8(b, idx);
    __m128i sel = _mm_cmpeq_epi8(_mm_and_si128(c, _mm_set1_epi8(16)), _mm_set1_epi8(16));
    return _mm_blendv_epi8(ra, rb, sel);
}
inline v128 lvsl(uint32_t ea) { v128 r; uint32_t sh = ea & 15; for (int i = 0; i < 16; i++) r.u8[15 - i] = uint8_t(sh + i); return r; }
inline v128 lvsr(uint32_t ea) { v128 r; uint32_t sh = ea & 15; for (int i = 0; i < 16; i++) r.u8[15 - i] = uint8_t(16 - sh + i); return r; }
inline v128 lvlx(uint8_t* b, uint32_t ea) {
    v128 r{};
    uint32_t sh = ea & 15;
    for (uint32_t i = 0; i < 16 - sh; i++) r.u8[15 - i] = b[ea + i];
    return r;
}
inline v128 lvrx(uint8_t* b, uint32_t ea) {
    v128 r{};
    uint32_t sh = ea & 15;
    uint32_t blk = ea & ~15u;
    for (uint32_t i = 0; i < sh; i++) r.u8[sh - 1 - i] = b[blk + i];
    return r;
}
inline void stvlx(uint8_t* b, uint32_t ea, const v128& v) {
    uint32_t sh = ea & 15;
    for (uint32_t i = 0; i < 16 - sh; i++) b[ea + i] = v.u8[15 - i];
}
inline void stvrx(uint8_t* b, uint32_t ea, const v128& v) {
    uint32_t sh = ea & 15;
    uint32_t blk = ea & ~15u;
    for (uint32_t i = 0; i < sh; i++) b[blk + i] = v.u8[sh - 1 - i];
}
inline void vcr6(Context& c, __m128i m) {
    int bits = _mm_movemask_epi8(m);
    c.cr[6] = {uint8_t(bits == 0xFFFF), 0, uint8_t(bits == 0), 0};
}
inline void dcbz(uint8_t* b, uint32_t ea) { std::memset(b + (ea & ~127u), 0, 128); notify_store(b, ea & ~127u, 128); }

}
