#pragma once

#include "../edge/edge_cull_ops.h"

#include <bit>
#include <cstdint>
#include <memory>

namespace cs::xo {

using edge::xo::V;
using edge::xo::F;
using edge::xo::I;
using edge::xo::a;
using edge::xo::addx;
using edge::xo::ah;
using edge::xo::ahi;
using edge::xo::ai;
using edge::xo::and_;
using edge::xo::andbi;
using edge::xo::andc;
using edge::xo::andhi;
using edge::xo::andi;
using edge::xo::bswap;
using edge::xo::cbd;
using edge::xo::cdd;
using edge::xo::ceq;
using edge::xo::ceqb;
using edge::xo::ceqbi;
using edge::xo::ceqh;
using edge::xo::ceqhi;
using edge::xo::ceqi;
using edge::xo::cg;
using edge::xo::cgt;
using edge::xo::cgtb;
using edge::xo::cgtbi;
using edge::xo::cgth;
using edge::xo::cgthi;
using edge::xo::cgti;
using edge::xo::chd;
using edge::xo::clgt;
using edge::xo::clgtb;
using edge::xo::clgtbi;
using edge::xo::clgth;
using edge::xo::clgthi;
using edge::xo::clgti;
using edge::xo::cntb;
using edge::xo::cwd;
using edge::xo::eqv;
using edge::xo::fa;
using edge::xo::fi;
using edge::xo::fkey;
using edge::xo::fm;
using edge::xo::fma;
using edge::xo::fmag;
using edge::xo::fms;
using edge::xo::fnms;
using edge::xo::frest;
using edge::xo::frsqest;
using edge::xo::fs;
using edge::xo::fsmb;
using edge::xo::fsmbi;
using edge::xo::gb;
using edge::xo::gbh;
using edge::xo::hw6;
using edge::xo::il;
using edge::xo::ila;
using edge::xo::ilh;
using edge::xo::ilhu;
using edge::xo::insert_mask;
using edge::xo::iohl;
using edge::xo::iota8;
using edge::xo::lq;
using edge::xo::mpy;
using edge::xo::mpyh;
using edge::xo::mpyhh;
using edge::xo::mpyhhu;
using edge::xo::mpyi;
using edge::xo::mpyu;
using edge::xo::mpyui;
using edge::xo::nand;
using edge::xo::nor;
using edge::xo::or_;
using edge::xo::orbi;
using edge::xo::orc;
using edge::xo::orhi;
using edge::xo::ori;
using edge::xo::pref;
using edge::xo::pref_only;
using edge::xo::qrot_bytes;
using edge::xo::qshl_bytes;
using edge::xo::qshr_bytes;
using edge::xo::rot;
using edge::xo::roth;
using edge::xo::rothm;
using edge::xo::rothmi;
using edge::xo::roti;
using edge::xo::rotm;
using edge::xo::rotma;
using edge::xo::rotmah;
using edge::xo::rotmahi;
using edge::xo::rotmai;
using edge::xo::rotmi;
using edge::xo::rotqby;
using edge::xo::rotqbyi;
using edge::xo::rotqmby;
using edge::xo::rotqmbyi;
using edge::xo::s16;
using edge::xo::s32;
using edge::xo::s8;
using edge::xo::selb;
using edge::xo::sf;
using edge::xo::sfh;
using edge::xo::sfhi;
using edge::xo::sfi;
using edge::xo::shl;
using edge::xo::shlhi;
using edge::xo::shli;
using edge::xo::shlqby;
using edge::xo::shlqbyi;
using edge::xo::shufb;
using edge::xo::smax_fix;
using edge::xo::sq;
using edge::xo::xor_;
using edge::xo::xorbi;
using edge::xo::xorhi;
using edge::xo::xori;
using edge::xo::xsbh;
using edge::xo::xshw;
using edge::xo::xswd;

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

#define CSX inline __attribute__((always_inline))

CSX bool journal(const uint8_t* ls, uint32_t q, Ctl& ctl) {
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

CSX void st(uint8_t* ls, uint32_t a, V v, Ctl& ctl) {
    a &= 0x3fff0;
    if (ctl.journal && !journal(ls, a, ctl)) [[unlikely]]
        return;
    _mm_storeu_si128(reinterpret_cast<__m128i*>(ls + a), bswap(v));
}

CSX uint32_t ld16(const uint8_t* ls, uint32_t a) { return (uint32_t(ls[a & 0x3ffff]) << 8) | ls[(a + 1) & 0x3ffff]; }

CSX void st32(uint8_t* ls, uint32_t a, uint32_t v, Ctl& ctl) {
    uint32_t q = a & 0x3fff0;
    if (ctl.journal && !journal(ls, q, ctl)) [[unlikely]]
        return;
    uint32_t be = __builtin_bswap32(v);
    std::memcpy(ls + q + (a & 0xc), &be, 4);
}

CSX V cbx(V x, V y) { return insert_mask(pref(x) + pref(y), 1); }
CSX V chx(V x, V y) { return insert_mask(pref(x) + pref(y), 2); }
CSX V cwx(V x, V y) { return insert_mask(pref(x) + pref(y), 4); }
CSX V cdx(V x, V y) { return insert_mask(pref(x) + pref(y), 8); }
CSX V gbb(V x) { return pref_only(uint32_t(_mm_movemask_epi8(_mm_slli_epi16(x, 7)))); }
CSX V orx(V x) {
    V t = _mm_or_si128(x, _mm_shuffle_epi32(x, 0x4e));
    t = _mm_or_si128(t, _mm_shuffle_epi32(t, 0xb1));
    return pref_only(uint32_t(_mm_cvtsi128_si32(t)));
}
CSX V clz(V x) {
    __m128i t = _mm_and_si128(x, s32(0x7fffffffu));
    __m128i e = _mm_srli_epi32(_mm_castps_si128(_mm_cvtepi32_ps(t)), 23);
    __m128i r = _mm_min_epi32(_mm_sub_epi32(s32(158), e), s32(32));
    return _mm_andnot_si128(_mm_srai_epi32(x, 31), r);
}
CSX V rothi(V x, int32_t i) {
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
CSX V bg(V x, V y) { return _mm_srli_epi32(_mm_xor_si128(clgt(x, y), s32(0xffffffffu)), 31); }
CSX V sfx(V x, V y, V t) { return _mm_sub_epi32(_mm_sub_epi32(y, x), _mm_xor_si128(_mm_and_si128(t, s32(1)), s32(1))); }
CSX V cgx(V x, V y, V t) {
    V sum = _mm_add_epi32(x, y);
    V c1 = cg(x, y);
    V c2 = _mm_and_si128(_mm_cmpeq_epi32(sum, s32(0xffffffffu)), _mm_and_si128(t, s32(1)));
    return _mm_or_si128(c1, c2);
}
CSX V bgx(V x, V y, V t) {
    V gt = clgt(y, x);
    V eq = _mm_cmpeq_epi32(x, y);
    V r = _mm_or_si128(gt, _mm_and_si128(eq, _mm_cmpeq_epi32(_mm_and_si128(t, s32(1)), s32(1))));
    return _mm_and_si128(r, s32(1));
}
CSX V mpys(V x, V y) { return _mm_srai_epi32(mpy(x, y), 16); }
CSX V mpya(V x, V y, V z) { return _mm_add_epi32(mpy(x, y), z); }
CSX bool any_nan(__m128 x, __m128 y) {
    __m128 u = _mm_cmpunord_ps(x, y);
    return !_mm_testz_ps(u, u);
}
CSX V fcgt(V x, V y) {
    if (any_nan(F(x), F(y))) [[unlikely]]
        return edge::xo::fcgt(x, y);
    return I(_mm_cmpgt_ps(F(x), F(y)));
}
CSX V fceq(V x, V y) {
    if (any_nan(F(x), F(y))) [[unlikely]]
        return edge::xo::fceq(x, y);
    return I(_mm_cmpeq_ps(F(x), F(y)));
}
CSX V fcmgt(V x, V y) {
    if (any_nan(F(x), F(y))) [[unlikely]]
        return edge::xo::fcmgt(x, y);
    __m128 m = _mm_castsi128_ps(s32(0x7fffffffu));
    return I(_mm_cmpgt_ps(_mm_and_ps(F(x), m), _mm_and_ps(F(y), m)));
}
CSX V fcmeq(V x, V y) {
    if (any_nan(F(x), F(y))) [[unlikely]]
        return edge::xo::fcmeq(x, y);
    __m128 m = _mm_castsi128_ps(s32(0x7fffffffu));
    return I(_mm_cmpeq_ps(_mm_and_ps(F(x), m), _mm_and_ps(F(y), m)));
}
CSX V qshl_bits(V x, uint32_t n) {
    if (!n) return x;
    __m128i c = _mm_cvtsi32_si128(int(n)), d = _mm_cvtsi32_si128(int(64 - n));
    return _mm_or_si128(_mm_sll_epi64(x, c), _mm_srl_epi64(_mm_bslli_si128(x, 8), d));
}
CSX V qshr_bits(V x, uint32_t n) {
    if (!n) return x;
    __m128i c = _mm_cvtsi32_si128(int(n)), d = _mm_cvtsi32_si128(int(64 - n));
    return _mm_or_si128(_mm_srl_epi64(x, c), _mm_sll_epi64(_mm_bsrli_si128(x, 8), d));
}
CSX V qrot_bits(V x, uint32_t n) {
    if (!n) return x;
    __m128i c = _mm_cvtsi32_si128(int(n)), d = _mm_cvtsi32_si128(int(64 - n));
    return _mm_or_si128(_mm_sll_epi64(x, c), _mm_srl_epi64(_mm_shuffle_epi32(x, 0x4e), d));
}
CSX V shlqbii(V x, int32_t i) { return qshl_bits(x, uint32_t(i) & 7); }
CSX V fsm(V x) {
    V m = s32(pref(x));
    V bits = _mm_setr_epi32(1, 2, 4, 8);
    return _mm_cmpeq_epi32(_mm_and_si128(m, bits), bits);
}
CSX V fsmh(V x) {
    V m = s16(pref(x));
    V bits = _mm_setr_epi16(1, 2, 4, 8, 16, 32, 64, 128);
    return _mm_cmpeq_epi16(_mm_and_si128(m, bits), bits);
}
CSX __m128 pow2f(int e) { return _mm_castsi128_ps(s32(uint32_t(127 + e) << 23)); }
inline V map_f(V x, uint32_t (*f)(uint32_t, int), int i8) {
    alignas(16) uint32_t a[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(a), x);
    for (auto& v : a) v = f(v, i8);
    return _mm_load_si128(reinterpret_cast<const __m128i*>(a));
}
inline uint32_t csflt1(uint32_t v, int i8) { return spuf::csflt(int32_t(v), i8); }
inline uint32_t cuflt1(uint32_t v, int i8) { return spuf::cuflt(v, i8); }
inline uint32_t cflts1(uint32_t v, int i8) { return uint32_t(spuf::cflts(v, i8)); }
inline uint32_t cfltu1(uint32_t v, int i8) { return spuf::cfltu(v, i8); }
CSX V csflt(V x, int i8) {
    if (i8 < 29 || i8 > 251) [[unlikely]]
        return map_f(x, csflt1, i8);
    __m128 f = _mm_cvtepi32_ps(x);
    if (i8 != 155) f = _mm_mul_ps(f, pow2f(i8 - 155));
    return I(f);
}
CSX V cuflt(V x, int i8) {
    if (i8 < 29 || i8 > 250) [[unlikely]]
        return map_f(x, cuflt1, i8);
    __m128 hi = _mm_mul_ps(_mm_cvtepi32_ps(_mm_srli_epi32(x, 16)), _mm_set1_ps(65536.0f));
    __m128 f = _mm_add_ps(hi, _mm_cvtepi32_ps(_mm_and_si128(x, s32(0xffffu))));
    if (i8 != 155) f = _mm_mul_ps(f, pow2f(i8 - 155));
    return I(f);
}
CSX V cflts(V x, int i8) {
    if (i8 < 46 || i8 > 299) [[unlikely]]
        return map_f(x, cflts1, i8);
    V s = i8 == 173 ? x : I(_mm_mul_ps(F(x), pow2f(173 - i8)));
    __m128i r = _mm_cvttps_epi32(F(s));
    __m128i big = _mm_cmpgt_epi32(_mm_and_si128(s, s32(0x7FFFFFFF)), s32(0x4EFFFFFF));
    __m128i sat = _mm_xor_si128(s32(0x7FFFFFFF), _mm_srai_epi32(s, 31));
    return _mm_blendv_epi8(r, sat, big);
}
CSX V cfltu(V x, int i8) {
    if (i8 < 46 || i8 > 299) [[unlikely]]
        return map_f(x, cfltu1, i8);
    V s = i8 == 173 ? x : I(_mm_mul_ps(F(x), pow2f(173 - i8)));
    __m128i neg = _mm_srai_epi32(s, 31);
    __m128i huge = _mm_cmpgt_epi32(s, s32(0x4F7FFFFF));
    __m128i hi = _mm_cmpgt_epi32(s, s32(0x4EFFFFFF));
    __m128i lo = _mm_cvttps_epi32(F(s));
    __m128i up = _mm_xor_si128(_mm_cvttps_epi32(_mm_sub_ps(F(s), _mm_set1_ps(2147483648.0f))), s32(0x80000000u));
    __m128i r = _mm_blendv_epi8(lo, up, hi);
    r = _mm_or_si128(r, huge);
    return _mm_andnot_si128(neg, r);
}
CSX V shlqbi(V x, V y) { return qshl_bits(x, pref(y) & 7); }
CSX V rotqbi(V x, V y) { return qrot_bits(x, pref(y) & 7); }
CSX V rotqbii(V x, int32_t i) { return qrot_bits(x, uint32_t(i) & 7); }
CSX V rotqmbi(V x, V y) { return qshr_bits(x, (0u - pref(y)) & 7); }
CSX V rotqmbii(V x, int32_t i) { return qshr_bits(x, (0u - uint32_t(i)) & 7); }
CSX V shlqbybi(V x, V y) { return qshl_bytes(x, (pref(y) >> 3) & 0x1f); }
CSX V rotqbybi(V x, V y) { return qrot_bytes(x, (pref(y) >> 3) & 0xf); }
CSX V rotqmbybi(V x, V y) { return qshr_bytes(x, (0u - (pref(y) >> 3)) & 0x1f); }

#undef CSX

}
