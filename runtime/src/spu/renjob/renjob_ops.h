#pragma once

#include "../edge/edge_cull_ops.h"
#include "renjob_chan.h"

#include <bit>
#include <cstdint>
#include <memory>

#define RJ_ABI __attribute__((sysv_abi))

namespace renjob::xo {

using namespace edge::xo;

struct JEntry {
    __m128i old;
    uint32_t a;
};

struct Ctl {
    static constexpr uint32_t kCap = 1u << 14;
    bool fail = false;
    bool out = false;
    bool journal = false;
    uint32_t fail_pc = 0;
    uint64_t budget = 1ull << 26;
    uint32_t jn = 0;
    JEntry* j = nullptr;
    uint64_t* dirty = nullptr;
    renjob::ChanHost* host = nullptr;
    bool* ie = nullptr;
    uint32_t icall_miss = 0;
    void clear_dirty() {
        for (uint32_t i = 0; i < jn; i++) dirty[j[i].a >> 10] = 0;
        jn = 0;
    }
    void reset(uint64_t b = 1ull << 26) {
        fail = false;
        out = false;
        budget = b;
        if (journal) clear_dirty();
    }
    void rollback(uint8_t* ls) {
        for (uint32_t i = jn; i-- > 0;) _mm_storeu_si128(reinterpret_cast<__m128i*>(ls + j[i].a), j[i].old);
        clear_dirty();
    }
};

struct JournalStore {
    std::unique_ptr<JEntry[]> j{new JEntry[Ctl::kCap]};
    std::unique_ptr<uint64_t[]> dirty{new uint64_t[0x40000 / 16 / 64]()};
    void attach(Ctl& c) {
        c.j = j.get();
        c.dirty = dirty.get();
        c.journal = true;
    }
};

#define RJ inline __attribute__((always_inline))

RJ bool journal(const uint8_t* ls, uint32_t q, Ctl& ctl) {
    uint64_t& w = ctl.dirty[q >> 10];
    uint64_t bit = 1ull << ((q >> 4) & 63);
    if (w & bit) [[likely]]
        return true;
    if (ctl.jn >= Ctl::kCap) [[unlikely]] {
        ctl.fail = true;
        return false;
    }
    w |= bit;
    JEntry& e = ctl.j[ctl.jn++];
    e.old = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ls + q));
    e.a = q;
    return true;
}

RJ void st(uint8_t* ls, uint32_t a, V v, Ctl& ctl) {
    a &= 0x3fff0;
    if (ctl.journal && !journal(ls, a, ctl)) [[unlikely]]
        return;
    _mm_storeu_si128(reinterpret_cast<__m128i*>(ls + a), bswap(v));
}

RJ uint32_t ld16(const uint8_t* ls, uint32_t a) { return (uint32_t(ls[a & 0x3ffff]) << 8) | ls[(a + 1) & 0x3ffff]; }

RJ void st32(uint8_t* ls, uint32_t a, uint32_t v, Ctl& ctl) {
    uint32_t q = a & 0x3fff0;
    if (ctl.journal && !journal(ls, q, ctl)) [[unlikely]]
        return;
    uint32_t be = __builtin_bswap32(v);
    std::memcpy(ls + q + (a & 0xc), &be, 4);
}

struct InsMasks {
    alignas(16) V m[4][16];
    InsMasks() {
        const int sizes[4] = {1, 2, 4, 8};
        for (int k = 0; k < 4; k++)
            for (uint32_t t = 0; t < 16; t++) m[k][t] = edge::xo::insert_mask(t, sizes[k]);
    }
};

inline const InsMasks kInsMasks;

RJ V ins_mask(uint32_t t, int k) { return _mm_load_si128(&kInsMasks.m[k][t & 15]); }
RJ V cbd(V x, int32_t i) { return ins_mask(pref(x) + uint32_t(i), 0); }
RJ V chd(V x, int32_t i) { return ins_mask(pref(x) + uint32_t(i), 1); }
RJ V cwd(V x, int32_t i) { return ins_mask(pref(x) + uint32_t(i), 2); }
RJ V cdd(V x, int32_t i) { return ins_mask(pref(x) + uint32_t(i), 3); }
RJ V cbx(V x, V y) { return ins_mask(pref(x) + pref(y), 0); }
RJ V chx(V x, V y) { return ins_mask(pref(x) + pref(y), 1); }
RJ V cwx(V x, V y) { return ins_mask(pref(x) + pref(y), 2); }
RJ V cdx(V x, V y) { return ins_mask(pref(x) + pref(y), 3); }

RJ void stp(uint8_t* ls, uint32_t a, V v, Ctl&) { _mm_storeu_si128(reinterpret_cast<__m128i*>(ls + (a & 0x3fff0)), bswap(v)); }

RJ void st32p(uint8_t* ls, uint32_t a, uint32_t v, Ctl&) {
    uint32_t be = __builtin_bswap32(v);
    std::memcpy(ls + (a & 0x3fff0) + (a & 0xc), &be, 4);
}
RJ V gbb(V x) { return pref_only(uint32_t(_mm_movemask_epi8(_mm_slli_epi16(x, 7)))); }
RJ V orx(V x) {
    V t = _mm_or_si128(x, _mm_shuffle_epi32(x, 0x4e));
    t = _mm_or_si128(t, _mm_shuffle_epi32(t, 0xb1));
    return pref_only(uint32_t(_mm_cvtsi128_si32(t)));
}
inline V clz(V x) {
    alignas(16) uint32_t a[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(a), x);
    for (auto& v : a) v = uint32_t(std::countl_zero(v));
    return _mm_load_si128(reinterpret_cast<const __m128i*>(a));
}
RJ V rothi(V x, int32_t i) {
    int s = i & 0xf;
    if (!s) return x;
    return _mm_or_si128(_mm_sll_epi16(x, _mm_cvtsi32_si128(s)), _mm_srl_epi16(x, _mm_cvtsi32_si128(16 - s)));
}
inline V shlh(V x, V y) {
    alignas(16) uint16_t a[8], b[8];
    _mm_store_si128(reinterpret_cast<__m128i*>(a), x);
    _mm_store_si128(reinterpret_cast<__m128i*>(b), y);
    for (int i = 0; i < 8; i++) {
        uint32_t s = b[i] & 0x1f;
        a[i] = uint16_t(s > 15 ? 0 : a[i] << s);
    }
    return _mm_load_si128(reinterpret_cast<const __m128i*>(a));
}
RJ V bg(V x, V y) { return _mm_srli_epi32(_mm_xor_si128(clgt(x, y), s32(0xffffffffu)), 31); }
RJ V sfx(V x, V y, V t) { return _mm_sub_epi32(_mm_sub_epi32(y, x), _mm_xor_si128(_mm_and_si128(t, s32(1)), s32(1))); }
inline V cgx(V x, V y, V t) {
    alignas(16) uint32_t a[4], b[4], c[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(a), x);
    _mm_store_si128(reinterpret_cast<__m128i*>(b), y);
    _mm_store_si128(reinterpret_cast<__m128i*>(c), t);
    for (int i = 0; i < 4; i++) c[i] = uint32_t(uint64_t(a[i]) + b[i] + (c[i] & 1) > 0xffffffffull);
    return _mm_load_si128(reinterpret_cast<const __m128i*>(c));
}
inline V bgx(V x, V y, V t) {
    alignas(16) uint32_t a[4], b[4], c[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(a), x);
    _mm_store_si128(reinterpret_cast<__m128i*>(b), y);
    _mm_store_si128(reinterpret_cast<__m128i*>(c), t);
    for (int i = 0; i < 4; i++) c[i] = uint32_t(int64_t(b[i]) - int64_t(a[i]) - (1 - (c[i] & 1)) >= 0);
    return _mm_load_si128(reinterpret_cast<const __m128i*>(c));
}
RJ V mpys(V x, V y) { return _mm_srai_epi32(mpy(x, y), 16); }
RJ V mpya(V x, V y, V z) { return _mm_add_epi32(mpy(x, y), z); }
inline V qshr_bits(V x, uint32_t n) {
    if (!n) return x;
    alignas(16) uint64_t q[2], r[2];
    _mm_store_si128(reinterpret_cast<__m128i*>(q), x);
    r[0] = (q[0] >> n) | (q[1] << (64 - n));
    r[1] = q[1] >> n;
    return _mm_load_si128(reinterpret_cast<const __m128i*>(r));
}
inline V qrot_bits(V x, uint32_t n) {
    if (!n) return x;
    alignas(16) uint64_t q[2], r[2];
    _mm_store_si128(reinterpret_cast<__m128i*>(q), x);
    r[1] = (q[1] << n) | (q[0] >> (64 - n));
    r[0] = (q[0] << n) | (q[1] >> (64 - n));
    return _mm_load_si128(reinterpret_cast<const __m128i*>(r));
}
struct ByteShiftTables {
    alignas(16) V rot[16];
    alignas(16) V shl[32];
    alignas(16) V shr[32];
    ByteShiftTables() {
        for (int n = 0; n < 16; n++) {
            alignas(16) int8_t b[16];
            for (int i = 0; i < 16; i++) b[i] = int8_t((i - n) & 15);
            rot[n] = _mm_load_si128(reinterpret_cast<const __m128i*>(b));
        }
        for (int n = 0; n < 32; n++) {
            alignas(16) int8_t a[16], c[16];
            for (int i = 0; i < 16; i++) {
                a[i] = i - n >= 0 ? int8_t(i - n) : int8_t(-128);
                c[i] = i + n <= 15 ? int8_t(i + n) : int8_t(-128);
            }
            shl[n] = _mm_load_si128(reinterpret_cast<const __m128i*>(a));
            shr[n] = _mm_load_si128(reinterpret_cast<const __m128i*>(c));
        }
    }
};

inline const ByteShiftTables kByteShift;

RJ V rotqby(V x, V y) { return _mm_shuffle_epi8(x, _mm_load_si128(&kByteShift.rot[pref(y) & 15])); }
RJ V shlqby(V x, V y) { return _mm_shuffle_epi8(x, _mm_load_si128(&kByteShift.shl[pref(y) & 31])); }
RJ V rotqmby(V x, V y) { return _mm_shuffle_epi8(x, _mm_load_si128(&kByteShift.shr[(0u - pref(y)) & 31])); }

RJ V shlqbi(V x, V y) { return qshl_bits(x, pref(y) & 7); }
RJ V rotqbi(V x, V y) { return qrot_bits(x, pref(y) & 7); }
RJ V rotqbii(V x, int32_t i) { return qrot_bits(x, uint32_t(i) & 7); }
RJ V rotqmbi(V x, V y) { return qshr_bits(x, (0u - pref(y)) & 7); }
RJ V rotqmbii(V x, int32_t i) { return qshr_bits(x, (0u - uint32_t(i)) & 7); }
RJ V shlqbybi(V x, V y) { return _mm_shuffle_epi8(x, _mm_load_si128(&kByteShift.shl[(pref(y) >> 3) & 31])); }
RJ V rotqbybi(V x, V y) { return _mm_shuffle_epi8(x, _mm_load_si128(&kByteShift.rot[(pref(y) >> 3) & 15])); }
RJ V rotqmbybi(V x, V y) { return _mm_shuffle_epi8(x, _mm_load_si128(&kByteShift.shr[(0u - (pref(y) >> 3)) & 31])); }

#undef RJ

}
