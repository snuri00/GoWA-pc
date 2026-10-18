#pragma once

#include "spu/context.h"
#include "ppu/ops.h"
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <limits>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"

#define SPU_INLINE inline __attribute__((always_inline))

namespace spu {

namespace {


using Handler = bool (*)(Context&, Host&, uint32_t, uint32_t);

SPU_INLINE int32_t sext(uint32_t v, int bits) { return static_cast<int32_t>(v << (32 - bits)) >> (32 - bits); }
SPU_INLINE uint32_t RT(uint32_t w) { return w & 0x7F; }
SPU_INLINE uint32_t RA(uint32_t w) { return (w >> 7) & 0x7F; }
SPU_INLINE uint32_t RB(uint32_t w) { return (w >> 14) & 0x7F; }
SPU_INLINE uint32_t RRR_T(uint32_t w) { return (w >> 21) & 0x7F; }
SPU_INLINE uint32_t RRR_C(uint32_t w) { return w & 0x7F; }
SPU_INLINE int32_t I7(uint32_t w) { return sext((w >> 14) & 0x7F, 7); }
SPU_INLINE uint32_t I8(uint32_t w) { return (w >> 14) & 0xFF; }
SPU_INLINE int32_t I10(uint32_t w) { return sext((w >> 14) & 0x3FF, 10); }
SPU_INLINE uint32_t I16(uint32_t w) { return (w >> 7) & 0xFFFF; }
SPU_INLINE int32_t S16(uint32_t w) { return sext((w >> 7) & 0xFFFF, 16); }
SPU_INLINE uint32_t I18(uint32_t w) { return (w >> 7) & 0x3FFFF; }

SPU_INLINE v128 load_ls(const Context& c, uint32_t addr) {
    v128 r;
    r.vi = ppu::bswap128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(c.ls + (addr & LS_MASK))));
    return r;
}

SPU_INLINE void store_ls(Context& c, uint32_t addr, const v128& v) {
    _mm_storeu_si128(reinterpret_cast<__m128i*>(c.ls + (addr & LS_MASK)), ppu::bswap128(v.vi));
    if (c.line_epoch) c.line_epoch[(addr & LS_MASK) >> 7].store(c.epoch->load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

SPU_INLINE bool region_valid(Context& c, uint32_t id, uint32_t lsa, uint32_t words, const uint32_t* code) {
    uint32_t v = c.block_valid[id];
    if (v && c.all_epoch->load(std::memory_order_acquire) <= v) {
        bool fresh = true;
        for (uint32_t l = lsa >> 7, last = (lsa + words * 4 - 1) >> 7; l <= last; l++) {
            if (c.line_epoch[l].load(std::memory_order_acquire) > v) {
                fresh = false;
                break;
            }
        }
        if (fresh) return true;
    }
    uint32_t e = c.epoch->load(std::memory_order_acquire);
    if (std::memcmp(c.ls + lsa, code, words * 4) != 0) return false;
    c.block_valid[id] = e;
    return true;
}

SPU_INLINE v128 splat32(uint32_t v) { v128 r; r.vi = _mm_set1_epi32(static_cast<int32_t>(v)); return r; }
SPU_INLINE v128 splat16(uint16_t v) { v128 r; r.vi = _mm_set1_epi16(static_cast<int16_t>(v)); return r; }
SPU_INLINE v128 splat8(uint8_t v) { v128 r; r.vi = _mm_set1_epi8(static_cast<int8_t>(v)); return r; }
SPU_INLINE v128 pref_only(uint32_t v) { v128 r{}; r.u32[3] = v; return r; }

SPU_INLINE uint8_t be_byte(const v128& v, int i) { return v.u8[15 - i]; }
SPU_INLINE void set_be_byte(v128& v, int i, uint8_t b) { v.u8[15 - i] = b; }

#define G(n) c.gpr[n]

template <class F>
SPU_INLINE v128 map32(const v128& a, const v128& b, F f) { v128 r; for (int i = 0; i < 4; i++) r.u32[i] = f(a.u32[i], b.u32[i]); return r; }
template <class F>
SPU_INLINE v128 map16(const v128& a, const v128& b, F f) { v128 r; for (int i = 0; i < 8; i++) r.u16[i] = f(a.u16[i], b.u16[i]); return r; }
template <class F>
SPU_INLINE v128 map8(const v128& a, const v128& b, F f) { v128 r; for (int i = 0; i < 16; i++) r.u8[i] = f(a.u8[i], b.u8[i]); return r; }

struct RoundGuard {
    unsigned old;
    RoundGuard() : old(_mm_getcsr()) { _mm_setcsr((old & ~0x6000u) & ~0x8040u); }
    ~RoundGuard() { _mm_setcsr(old); }
};

SPU_INLINE float fzero_mask(float r, float a, float b) {
    return (a == 0.0f || b == 0.0f) ? 0.0f * std::copysign(1.0f, a) * std::copysign(1.0f, b) : r;
}

SPU_INLINE uint32_t smax_fix(uint32_t bits) {
    uint32_t e = bits & 0x7F800000;
    if (e == 0x7F800000) return (bits & 0x80000000) | 0x7FFFFFFF;
    return bits;
}

SPU_INLINE float fclamp(float x) { return std::bit_cast<float>(smax_fix(std::bit_cast<uint32_t>(x))); }

SPU_INLINE bool op_stop(Context& c, Host& h, uint32_t w, uint32_t pc) {
    c.pc = pc + 4;
    return h.stop_and_signal(w & 0x3FFF);
}
SPU_INLINE bool op_nop(Context&, Host&, uint32_t, uint32_t) { return true; }
SPU_INLINE bool op_rdch(Context& c, Host& h, uint32_t w, uint32_t pc) {
    uint32_t v = h.read_channel(RA(w));
    G(RT(w)) = pref_only(v);
    return !c.halted;
}
SPU_INLINE bool op_rchcnt(Context& c, Host& h, uint32_t w, uint32_t) { G(RT(w)) = pref_only(h.channel_count(RA(w))); return true; }
SPU_INLINE bool op_wrch(Context& c, Host& h, uint32_t w, uint32_t) { h.write_channel(RA(w), pref(G(RT(w)))); return !c.halted; }
SPU_INLINE bool op_mfspr(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = v128{}; return true; }
SPU_INLINE bool op_illegal(Context& c, Host& h, uint32_t w, uint32_t pc) { h.halt(pc, w); c.halted = true; return false; }

SPU_INLINE bool op_lqd(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = load_ls(c, pref(G(RA(w))) + (I10(w) << 4)); return true; }
SPU_INLINE bool op_lqx(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = load_ls(c, pref(G(RA(w))) + pref(G(RB(w)))); return true; }
SPU_INLINE bool op_lqa(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = load_ls(c, static_cast<uint32_t>(S16(w) << 2)); return true; }
SPU_INLINE bool op_lqr(Context& c, Host&, uint32_t w, uint32_t pc) { G(RT(w)) = load_ls(c, pc + (S16(w) << 2)); return true; }
SPU_INLINE bool op_stqd(Context& c, Host&, uint32_t w, uint32_t) { store_ls(c, pref(G(RA(w))) + (I10(w) << 4), G(RT(w))); return true; }
SPU_INLINE bool op_stqx(Context& c, Host&, uint32_t w, uint32_t) { store_ls(c, pref(G(RA(w))) + pref(G(RB(w))), G(RT(w))); return true; }
SPU_INLINE bool op_stqa(Context& c, Host&, uint32_t w, uint32_t) { store_ls(c, static_cast<uint32_t>(S16(w) << 2), G(RT(w))); return true; }
SPU_INLINE bool op_stqr(Context& c, Host&, uint32_t w, uint32_t pc) { store_ls(c, pc + (S16(w) << 2), G(RT(w))); return true; }

SPU_INLINE bool op_br(Context& c, Host&, uint32_t w, uint32_t pc) { c.pc = (pc + (S16(w) << 2)) & PC_MASK; return true; }
SPU_INLINE bool op_bra(Context& c, Host&, uint32_t w, uint32_t) { c.pc = static_cast<uint32_t>(S16(w) << 2) & PC_MASK; return true; }
SPU_INLINE bool op_brsl(Context& c, Host&, uint32_t w, uint32_t pc) { uint32_t t = (pc + (S16(w) << 2)) & PC_MASK; G(RT(w)) = pref_only((pc + 4) & PC_MASK); c.pc = t; return true; }
SPU_INLINE bool op_brasl(Context& c, Host&, uint32_t w, uint32_t pc) { uint32_t t = static_cast<uint32_t>(S16(w) << 2) & PC_MASK; G(RT(w)) = pref_only((pc + 4) & PC_MASK); c.pc = t; return true; }
SPU_INLINE bool op_brz(Context& c, Host&, uint32_t w, uint32_t pc) { if (pref(G(RT(w))) == 0) c.pc = (pc + (S16(w) << 2)) & PC_MASK; return true; }
SPU_INLINE bool op_brnz(Context& c, Host&, uint32_t w, uint32_t pc) { if (pref(G(RT(w))) != 0) c.pc = (pc + (S16(w) << 2)) & PC_MASK; return true; }
SPU_INLINE bool op_brhz(Context& c, Host&, uint32_t w, uint32_t pc) { if (G(RT(w)).u16[6] == 0) c.pc = (pc + (S16(w) << 2)) & PC_MASK; return true; }
SPU_INLINE bool op_brhnz(Context& c, Host&, uint32_t w, uint32_t pc) { if (G(RT(w)).u16[6] != 0) c.pc = (pc + (S16(w) << 2)) & PC_MASK; return true; }

SPU_INLINE void interrupt_bits(Context& c, uint32_t w) {
    if (w & 0x40000) {
        c.interrupts_enabled = true;
        c.irq_check = true;
    }
    if (w & 0x80000) c.interrupts_enabled = false;
}

SPU_INLINE bool op_bi(Context& c, Host&, uint32_t w, uint32_t) { c.pc = pref(G(RA(w))) & PC_MASK; interrupt_bits(c, w); return true; }
SPU_INLINE bool op_bisl(Context& c, Host&, uint32_t w, uint32_t pc) { uint32_t t = pref(G(RA(w))) & PC_MASK; G(RT(w)) = pref_only((pc + 4) & PC_MASK); c.pc = t; interrupt_bits(c, w); return true; }
SPU_INLINE bool op_iret(Context& c, Host&, uint32_t w, uint32_t) { c.pc = c.srr0 & PC_MASK; interrupt_bits(c, w); return true; }
SPU_INLINE bool op_bisled(Context& c, Host& h, uint32_t w, uint32_t pc) {
    uint32_t t = pref(G(RA(w))) & PC_MASK;
    G(RT(w)) = pref_only((pc + 4) & PC_MASK);
    if (h.channel_count(0)) c.pc = t;
    interrupt_bits(c, w);
    return true;
}
SPU_INLINE bool op_biz(Context& c, Host&, uint32_t w, uint32_t) { if (pref(G(RT(w))) == 0) { c.pc = pref(G(RA(w))) & PC_MASK; interrupt_bits(c, w); } return true; }
SPU_INLINE bool op_binz(Context& c, Host&, uint32_t w, uint32_t) { if (pref(G(RT(w))) != 0) { c.pc = pref(G(RA(w))) & PC_MASK; interrupt_bits(c, w); } return true; }
SPU_INLINE bool op_bihz(Context& c, Host&, uint32_t w, uint32_t) { if (G(RT(w)).u16[6] == 0) { c.pc = pref(G(RA(w))) & PC_MASK; interrupt_bits(c, w); } return true; }
SPU_INLINE bool op_bihnz(Context& c, Host&, uint32_t w, uint32_t) { if (G(RT(w)).u16[6] != 0) { c.pc = pref(G(RA(w))) & PC_MASK; interrupt_bits(c, w); } return true; }

SPU_INLINE bool op_il(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = splat32(static_cast<uint32_t>(S16(w))); return true; }
SPU_INLINE bool op_ilh(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = splat16(static_cast<uint16_t>(I16(w))); return true; }
SPU_INLINE bool op_ilhu(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = splat32(I16(w) << 16); return true; }
SPU_INLINE bool op_ila(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = splat32(I18(w)); return true; }
SPU_INLINE bool op_iohl(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_or_si128(G(RT(w)).vi, _mm_set1_epi32(static_cast<int32_t>(I16(w)))); return true; }
SPU_INLINE bool op_fsmbi(Context& c, Host&, uint32_t w, uint32_t) {
    uint32_t m = I16(w);
    v128 r;
    for (int i = 0; i < 16; i++) set_be_byte(r, i, (m >> (15 - i)) & 1 ? 0xFF : 0);
    G(RT(w)) = r;
    return true;
}

SPU_INLINE bool op_a(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_add_epi32(G(RA(w)).vi, G(RB(w)).vi); return true; }
SPU_INLINE bool op_ah(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_add_epi16(G(RA(w)).vi, G(RB(w)).vi); return true; }
SPU_INLINE bool op_ai(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_add_epi32(G(RA(w)).vi, _mm_set1_epi32(I10(w))); return true; }
SPU_INLINE bool op_ahi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_add_epi16(G(RA(w)).vi, _mm_set1_epi16(static_cast<int16_t>(I10(w)))); return true; }
SPU_INLINE bool op_sf(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_sub_epi32(G(RB(w)).vi, G(RA(w)).vi); return true; }
SPU_INLINE bool op_sfh(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_sub_epi16(G(RB(w)).vi, G(RA(w)).vi); return true; }
SPU_INLINE bool op_sfi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_sub_epi32(_mm_set1_epi32(I10(w)), G(RA(w)).vi); return true; }
SPU_INLINE bool op_sfhi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_sub_epi16(_mm_set1_epi16(static_cast<int16_t>(I10(w))), G(RA(w)).vi); return true; }
SPU_INLINE bool op_addx(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map32(G(RA(w)), G(RB(w)), [&, i = 0](uint32_t x, uint32_t y) mutable { return x + y + (G(RT(w)).u32[i++] & 1); }); return true; }
SPU_INLINE bool op_sfx(Context& c, Host&, uint32_t w, uint32_t) { v128 t = G(RT(w)); G(RT(w)) = map32(G(RA(w)), G(RB(w)), [&, i = 0](uint32_t x, uint32_t y) mutable { return y - x - (1 - (t.u32[i++] & 1)); }); return true; }
SPU_INLINE bool op_cg(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map32(G(RA(w)), G(RB(w)), [](uint32_t x, uint32_t y) { return uint32_t(uint64_t(x) + y > 0xFFFFFFFFull); }); return true; }
SPU_INLINE bool op_bg(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map32(G(RA(w)), G(RB(w)), [](uint32_t x, uint32_t y) { return uint32_t(y >= x); }); return true; }
SPU_INLINE bool op_cgx(Context& c, Host&, uint32_t w, uint32_t) { v128 t = G(RT(w)); G(RT(w)) = map32(G(RA(w)), G(RB(w)), [&, i = 0](uint32_t x, uint32_t y) mutable { return uint32_t(uint64_t(x) + y + (t.u32[i++] & 1) > 0xFFFFFFFFull); }); return true; }
SPU_INLINE bool op_bgx(Context& c, Host&, uint32_t w, uint32_t) { v128 t = G(RT(w)); G(RT(w)) = map32(G(RA(w)), G(RB(w)), [&, i = 0](uint32_t x, uint32_t y) mutable { int64_t r = int64_t(y) - int64_t(x) - (1 - (t.u32[i++] & 1)); return uint32_t(r >= 0); }); return true; }

SPU_INLINE bool op_mpy(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map32(G(RA(w)), G(RB(w)), [](uint32_t x, uint32_t y) { return uint32_t(int32_t(int16_t(x)) * int16_t(y)); }); return true; }
SPU_INLINE bool op_mpyu(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map32(G(RA(w)), G(RB(w)), [](uint32_t x, uint32_t y) { return uint32_t(uint16_t(x)) * uint16_t(y); }); return true; }
SPU_INLINE bool op_mpyi(Context& c, Host&, uint32_t w, uint32_t) { int32_t imm = I10(w); G(RT(w)) = map32(G(RA(w)), G(RA(w)), [imm](uint32_t x, uint32_t) { return uint32_t(int32_t(int16_t(x)) * int32_t(int16_t(imm))); }); return true; }
SPU_INLINE bool op_mpyui(Context& c, Host&, uint32_t w, uint32_t) { uint32_t imm = uint32_t(I10(w)) & 0xFFFF; G(RT(w)) = map32(G(RA(w)), G(RA(w)), [imm](uint32_t x, uint32_t) { return uint32_t(uint16_t(x)) * imm; }); return true; }
SPU_INLINE bool op_mpya(Context& c, Host&, uint32_t w, uint32_t) { v128 a = G(RA(w)), b = G(RB(w)), cc = G(RRR_C(w)); v128 r; for (int i = 0; i < 4; i++) r.u32[i] = uint32_t(int32_t(int16_t(a.u32[i])) * int16_t(b.u32[i])) + cc.u32[i]; G(RRR_T(w)) = r; return true; }
SPU_INLINE bool op_mpyh(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map32(G(RA(w)), G(RB(w)), [](uint32_t x, uint32_t y) { return ((x >> 16) * (y & 0xFFFF)) << 16; }); return true; }
SPU_INLINE bool op_mpys(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map32(G(RA(w)), G(RB(w)), [](uint32_t x, uint32_t y) { return uint32_t((int32_t(int16_t(x)) * int16_t(y)) >> 16); }); return true; }
SPU_INLINE bool op_mpyhh(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map32(G(RA(w)), G(RB(w)), [](uint32_t x, uint32_t y) { return uint32_t(int32_t(int16_t(x >> 16)) * int16_t(y >> 16)); }); return true; }
SPU_INLINE bool op_mpyhhu(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map32(G(RA(w)), G(RB(w)), [](uint32_t x, uint32_t y) { return (x >> 16) * (y >> 16); }); return true; }
SPU_INLINE bool op_mpyhha(Context& c, Host&, uint32_t w, uint32_t) { v128 t = G(RT(w)); G(RT(w)) = map32(G(RA(w)), G(RB(w)), [&, i = 0](uint32_t x, uint32_t y) mutable { return uint32_t(int32_t(int16_t(x >> 16)) * int16_t(y >> 16)) + t.u32[i++]; }); return true; }
SPU_INLINE bool op_mpyhhau(Context& c, Host&, uint32_t w, uint32_t) { v128 t = G(RT(w)); G(RT(w)) = map32(G(RA(w)), G(RB(w)), [&, i = 0](uint32_t x, uint32_t y) mutable { return (x >> 16) * (y >> 16) + t.u32[i++]; }); return true; }

SPU_INLINE bool op_clz(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map32(G(RA(w)), G(RA(w)), [](uint32_t x, uint32_t) { return uint32_t(std::countl_zero(x)); }); return true; }
SPU_INLINE bool op_cntb(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map8(G(RA(w)), G(RA(w)), [](uint8_t x, uint8_t) { return uint8_t(std::popcount(x)); }); return true; }
SPU_INLINE bool op_avgb(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_avg_epu8(G(RA(w)).vi, G(RB(w)).vi); return true; }
SPU_INLINE bool op_absdb(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map8(G(RA(w)), G(RB(w)), [](uint8_t x, uint8_t y) { return uint8_t(x > y ? x - y : y - x); }); return true; }
SPU_INLINE bool op_sumb(Context& c, Host&, uint32_t w, uint32_t) {
    v128 a = G(RA(w)), b = G(RB(w)), r;
    for (int i = 0; i < 4; i++) {
        uint16_t sb = 0, sa = 0;
        for (int k = 0; k < 4; k++) { sb += b.u8[i * 4 + k]; sa += a.u8[i * 4 + k]; }
        r.u16[i * 2 + 1] = sb;
        r.u16[i * 2] = sa;
    }
    G(RT(w)) = r;
    return true;
}
SPU_INLINE bool op_xsbh(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_srai_epi16(_mm_slli_epi16(G(RA(w)).vi, 8), 8); return true; }
SPU_INLINE bool op_xshw(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_srai_epi32(_mm_slli_epi32(G(RA(w)).vi, 16), 16); return true; }
SPU_INLINE bool op_xswd(Context& c, Host&, uint32_t w, uint32_t) { v128 a = G(RA(w)), r; for (int i = 0; i < 2; i++) r.u64[i] = uint64_t(int64_t(int32_t(a.u64[i]))); G(RT(w)) = r; return true; }

SPU_INLINE bool op_and(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_and_si128(G(RA(w)).vi, G(RB(w)).vi); return true; }
SPU_INLINE bool op_andc(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_andnot_si128(G(RB(w)).vi, G(RA(w)).vi); return true; }
SPU_INLINE bool op_or(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_or_si128(G(RA(w)).vi, G(RB(w)).vi); return true; }
SPU_INLINE bool op_orc(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_or_si128(G(RA(w)).vi, _mm_xor_si128(G(RB(w)).vi, _mm_set1_epi32(-1))); return true; }
SPU_INLINE bool op_xor(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_xor_si128(G(RA(w)).vi, G(RB(w)).vi); return true; }
SPU_INLINE bool op_nand(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_xor_si128(_mm_and_si128(G(RA(w)).vi, G(RB(w)).vi), _mm_set1_epi32(-1)); return true; }
SPU_INLINE bool op_nor(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_xor_si128(_mm_or_si128(G(RA(w)).vi, G(RB(w)).vi), _mm_set1_epi32(-1)); return true; }
SPU_INLINE bool op_eqv(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_xor_si128(_mm_xor_si128(G(RA(w)).vi, G(RB(w)).vi), _mm_set1_epi32(-1)); return true; }
SPU_INLINE bool op_andi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_and_si128(G(RA(w)).vi, _mm_set1_epi32(I10(w))); return true; }
SPU_INLINE bool op_andhi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_and_si128(G(RA(w)).vi, _mm_set1_epi16(static_cast<int16_t>(I10(w)))); return true; }
SPU_INLINE bool op_andbi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_and_si128(G(RA(w)).vi, _mm_set1_epi8(static_cast<int8_t>(I10(w)))); return true; }
SPU_INLINE bool op_ori(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_or_si128(G(RA(w)).vi, _mm_set1_epi32(I10(w))); return true; }
SPU_INLINE bool op_orhi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_or_si128(G(RA(w)).vi, _mm_set1_epi16(static_cast<int16_t>(I10(w)))); return true; }
SPU_INLINE bool op_orbi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_or_si128(G(RA(w)).vi, _mm_set1_epi8(static_cast<int8_t>(I10(w)))); return true; }
SPU_INLINE bool op_xori(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_xor_si128(G(RA(w)).vi, _mm_set1_epi32(I10(w))); return true; }
SPU_INLINE bool op_xorhi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_xor_si128(G(RA(w)).vi, _mm_set1_epi16(static_cast<int16_t>(I10(w)))); return true; }
SPU_INLINE bool op_xorbi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_xor_si128(G(RA(w)).vi, _mm_set1_epi8(static_cast<int8_t>(I10(w)))); return true; }
SPU_INLINE bool op_orx(Context& c, Host&, uint32_t w, uint32_t) { v128 a = G(RA(w)); G(RT(w)) = pref_only(a.u32[0] | a.u32[1] | a.u32[2] | a.u32[3]); return true; }
SPU_INLINE bool op_selb(Context& c, Host&, uint32_t w, uint32_t) {
    v128 m = G(RRR_C(w));
    G(RRR_T(w)).vi = _mm_or_si128(_mm_andnot_si128(m.vi, G(RA(w)).vi), _mm_and_si128(m.vi, G(RB(w)).vi));
    return true;
}
SPU_INLINE bool op_shufb(Context& c, Host&, uint32_t w, uint32_t) {
    __m128i ctl = G(RRR_C(w)).vi;
    __m128i idx = _mm_andnot_si128(ctl, _mm_set1_epi8(0x0F));
    __m128i ra = _mm_shuffle_epi8(G(RA(w)).vi, idx);
    __m128i rb = _mm_shuffle_epi8(G(RB(w)).vi, idx);
    __m128i r = _mm_blendv_epi8(ra, rb, _mm_slli_epi16(ctl, 3));
    __m128i t = _mm_and_si128(ctl, _mm_set1_epi8(static_cast<char>(0xE0)));
    __m128i special = _mm_or_si128(_mm_cmpeq_epi8(t, _mm_set1_epi8(static_cast<char>(0xC0))),
                                   _mm_and_si128(_mm_cmpeq_epi8(t, _mm_set1_epi8(static_cast<char>(0xE0))), _mm_set1_epi8(static_cast<char>(0x80))));
    G(RRR_T(w)).vi = _mm_blendv_epi8(r, special, ctl);
    return true;
}

SPU_INLINE bool op_fsmb(Context& c, Host&, uint32_t w, uint32_t) { uint32_t m = pref(G(RA(w))) & 0xFFFF; v128 r; for (int i = 0; i < 16; i++) set_be_byte(r, i, (m >> (15 - i)) & 1 ? 0xFF : 0); G(RT(w)) = r; return true; }
SPU_INLINE bool op_fsmh(Context& c, Host&, uint32_t w, uint32_t) { uint32_t m = pref(G(RA(w))) & 0xFF; v128 r; for (int i = 0; i < 8; i++) r.u16[7 - i] = (m >> (7 - i)) & 1 ? 0xFFFF : 0; G(RT(w)) = r; return true; }
SPU_INLINE bool op_fsm(Context& c, Host&, uint32_t w, uint32_t) { uint32_t m = pref(G(RA(w))) & 0xF; v128 r; for (int i = 0; i < 4; i++) r.u32[3 - i] = (m >> (3 - i)) & 1 ? 0xFFFFFFFF : 0; G(RT(w)) = r; return true; }
SPU_INLINE bool op_gbb(Context& c, Host&, uint32_t w, uint32_t) { v128 a = G(RA(w)); uint32_t m = 0; for (int i = 0; i < 16; i++) m = (m << 1) | (be_byte(a, i) & 1); G(RT(w)) = pref_only(m); return true; }
SPU_INLINE bool op_gbh(Context& c, Host&, uint32_t w, uint32_t) { v128 a = G(RA(w)); uint32_t m = 0; for (int i = 0; i < 8; i++) m = (m << 1) | (a.u16[7 - i] & 1); G(RT(w)) = pref_only(m); return true; }
SPU_INLINE bool op_gb(Context& c, Host&, uint32_t w, uint32_t) { v128 a = G(RA(w)); uint32_t m = 0; for (int i = 0; i < 4; i++) m = (m << 1) | (a.u32[3 - i] & 1); G(RT(w)) = pref_only(m); return true; }

SPU_INLINE bool op_shlh(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map16(G(RA(w)), G(RB(w)), [](uint16_t x, uint16_t y) { uint32_t s = y & 0x1F; return uint16_t(s > 15 ? 0 : x << s); }); return true; }
SPU_INLINE bool op_shlhi(Context& c, Host&, uint32_t w, uint32_t) { uint32_t s = uint32_t(I7(w)) & 0x1F; G(RT(w)).vi = s > 15 ? _mm_setzero_si128() : _mm_sll_epi16(G(RA(w)).vi, _mm_cvtsi32_si128(int(s))); return true; }
SPU_INLINE bool op_shl(Context& c, Host&, uint32_t w, uint32_t) { __m128i s = _mm_and_si128(G(RB(w)).vi, _mm_set1_epi32(0x3F)); G(RT(w)).vi = _mm_sllv_epi32(G(RA(w)).vi, s); return true; }
SPU_INLINE bool op_shli(Context& c, Host&, uint32_t w, uint32_t) { uint32_t s = uint32_t(I7(w)) & 0x3F; G(RT(w)).vi = s > 31 ? _mm_setzero_si128() : _mm_sll_epi32(G(RA(w)).vi, _mm_cvtsi32_si128(int(s))); return true; }
SPU_INLINE bool op_roth(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map16(G(RA(w)), G(RB(w)), [](uint16_t x, uint16_t y) { return std::rotl(x, y & 0xF); }); return true; }
SPU_INLINE bool op_rothi(Context& c, Host&, uint32_t w, uint32_t) { int s = I7(w) & 0xF; G(RT(w)) = map16(G(RA(w)), G(RA(w)), [s](uint16_t x, uint16_t) { return std::rotl(x, s); }); return true; }
SPU_INLINE bool op_rot(Context& c, Host&, uint32_t w, uint32_t) { __m128i s = _mm_and_si128(G(RB(w)).vi, _mm_set1_epi32(0x1F)); __m128i a = G(RA(w)).vi; G(RT(w)).vi = _mm_or_si128(_mm_sllv_epi32(a, s), _mm_srlv_epi32(a, _mm_sub_epi32(_mm_set1_epi32(32), s))); return true; }
SPU_INLINE bool op_roti(Context& c, Host&, uint32_t w, uint32_t) { int s = I7(w) & 0x1F; __m128i a = G(RA(w)).vi; G(RT(w)).vi = _mm_or_si128(_mm_sll_epi32(a, _mm_cvtsi32_si128(s)), _mm_srl_epi32(a, _mm_cvtsi32_si128(32 - s))); return true; }
SPU_INLINE bool op_rothm(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map16(G(RA(w)), G(RB(w)), [](uint16_t x, uint16_t y) { uint32_t s = (0u - y) & 0x1F; return uint16_t(s > 15 ? 0 : x >> s); }); return true; }
SPU_INLINE bool op_rothmi(Context& c, Host&, uint32_t w, uint32_t) { uint32_t s = (0u - uint32_t(I7(w))) & 0x1F; G(RT(w)) = map16(G(RA(w)), G(RA(w)), [s](uint16_t x, uint16_t) { return uint16_t(s > 15 ? 0 : x >> s); }); return true; }
SPU_INLINE bool op_rotm(Context& c, Host&, uint32_t w, uint32_t) { __m128i s = _mm_and_si128(_mm_sub_epi32(_mm_setzero_si128(), G(RB(w)).vi), _mm_set1_epi32(0x3F)); G(RT(w)).vi = _mm_srlv_epi32(G(RA(w)).vi, s); return true; }
SPU_INLINE bool op_rotmi(Context& c, Host&, uint32_t w, uint32_t) { uint32_t s = (0u - uint32_t(I7(w))) & 0x3F; G(RT(w)).vi = s > 31 ? _mm_setzero_si128() : _mm_srl_epi32(G(RA(w)).vi, _mm_cvtsi32_si128(int(s))); return true; }
SPU_INLINE bool op_rotmah(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map16(G(RA(w)), G(RB(w)), [](uint16_t x, uint16_t y) { uint32_t s = (0u - y) & 0x1F; return uint16_t(int16_t(x) >> (s > 15 ? 15 : s)); }); return true; }
SPU_INLINE bool op_rotmahi(Context& c, Host&, uint32_t w, uint32_t) { uint32_t s = (0u - uint32_t(I7(w))) & 0x1F; G(RT(w)) = map16(G(RA(w)), G(RA(w)), [s](uint16_t x, uint16_t) { return uint16_t(int16_t(x) >> (s > 15 ? 15 : s)); }); return true; }
SPU_INLINE bool op_rotma(Context& c, Host&, uint32_t w, uint32_t) { __m128i s = _mm_min_epu32(_mm_and_si128(_mm_sub_epi32(_mm_setzero_si128(), G(RB(w)).vi), _mm_set1_epi32(0x3F)), _mm_set1_epi32(31)); G(RT(w)).vi = _mm_srav_epi32(G(RA(w)).vi, s); return true; }
SPU_INLINE bool op_rotmai(Context& c, Host&, uint32_t w, uint32_t) { uint32_t s = (0u - uint32_t(I7(w))) & 0x3F; G(RT(w)).vi = _mm_sra_epi32(G(RA(w)).vi, _mm_cvtsi32_si128(int(s > 31 ? 31 : s))); return true; }

SPU_INLINE __m128i iota8() { return _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15); }
SPU_INLINE v128 qshl_bytes(const v128& a, uint32_t n) {
    v128 r;
    if (n >= 16) { r.vi = _mm_setzero_si128(); return r; }
    r.vi = _mm_shuffle_epi8(a.vi, _mm_sub_epi8(iota8(), _mm_set1_epi8(static_cast<char>(n))));
    return r;
}
SPU_INLINE v128 qshr_bytes(const v128& a, uint32_t n) {
    v128 r;
    if (n >= 16) { r.vi = _mm_setzero_si128(); return r; }
    __m128i idx = _mm_add_epi8(iota8(), _mm_set1_epi8(static_cast<char>(n)));
    idx = _mm_or_si128(idx, _mm_cmpgt_epi8(idx, _mm_set1_epi8(15)));
    r.vi = _mm_shuffle_epi8(a.vi, idx);
    return r;
}
SPU_INLINE v128 qrot_bytes(const v128& a, uint32_t n) {
    v128 r;
    r.vi = _mm_shuffle_epi8(a.vi, _mm_and_si128(_mm_sub_epi8(iota8(), _mm_set1_epi8(static_cast<char>(n & 15))), _mm_set1_epi8(15)));
    return r;
}
SPU_INLINE v128 qshl_bits(const v128& a, uint32_t n) { if (!n) return a; v128 r; r.u64[1] = (a.u64[1] << n) | (a.u64[0] >> (64 - n)); r.u64[0] = a.u64[0] << n; return r; }
SPU_INLINE v128 qshr_bits(const v128& a, uint32_t n) { if (!n) return a; v128 r; r.u64[0] = (a.u64[0] >> n) | (a.u64[1] << (64 - n)); r.u64[1] = a.u64[1] >> n; return r; }
SPU_INLINE v128 qrot_bits(const v128& a, uint32_t n) { if (!n) return a; v128 r; r.u64[1] = (a.u64[1] << n) | (a.u64[0] >> (64 - n)); r.u64[0] = (a.u64[0] << n) | (a.u64[1] >> (64 - n)); return r; }

SPU_INLINE bool op_shlqbi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qshl_bits(G(RA(w)), pref(G(RB(w))) & 7); return true; }
SPU_INLINE bool op_shlqbii(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qshl_bits(G(RA(w)), uint32_t(I7(w)) & 7); return true; }
SPU_INLINE bool op_shlqby(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qshl_bytes(G(RA(w)), pref(G(RB(w))) & 0x1F); return true; }
SPU_INLINE bool op_shlqbyi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qshl_bytes(G(RA(w)), uint32_t(I7(w)) & 0x1F); return true; }
SPU_INLINE bool op_shlqbybi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qshl_bytes(G(RA(w)), (pref(G(RB(w))) >> 3) & 0x1F); return true; }
SPU_INLINE bool op_rotqby(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qrot_bytes(G(RA(w)), pref(G(RB(w))) & 0xF); return true; }
SPU_INLINE bool op_rotqbyi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qrot_bytes(G(RA(w)), uint32_t(I7(w)) & 0xF); return true; }
SPU_INLINE bool op_rotqbybi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qrot_bytes(G(RA(w)), (pref(G(RB(w))) >> 3) & 0xF); return true; }
SPU_INLINE bool op_rotqbi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qrot_bits(G(RA(w)), pref(G(RB(w))) & 7); return true; }
SPU_INLINE bool op_rotqbii(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qrot_bits(G(RA(w)), uint32_t(I7(w)) & 7); return true; }
SPU_INLINE bool op_rotqmby(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qshr_bytes(G(RA(w)), (0u - pref(G(RB(w)))) & 0x1F); return true; }
SPU_INLINE bool op_rotqmbyi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qshr_bytes(G(RA(w)), (0u - uint32_t(I7(w))) & 0x1F); return true; }
SPU_INLINE bool op_rotqmbybi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qshr_bytes(G(RA(w)), (0u - (pref(G(RB(w))) >> 3)) & 0x1F); return true; }
SPU_INLINE bool op_rotqmbi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qshr_bits(G(RA(w)), (0u - pref(G(RB(w)))) & 7); return true; }
SPU_INLINE bool op_rotqmbii(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qshr_bits(G(RA(w)), (0u - uint32_t(I7(w))) & 7); return true; }

SPU_INLINE v128 insert_mask(uint32_t t, int size) {
    v128 r;
    for (int i = 0; i < 16; i++) set_be_byte(r, i, uint8_t(0x10 + i));
    uint32_t base = t & ~uint32_t(size - 1) & 0xF;
    static const uint8_t pat[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    if (size == 1) set_be_byte(r, int(t & 0xF), 0x03);
    else if (size == 2) { set_be_byte(r, int(base), 0x02); set_be_byte(r, int(base) + 1, 0x03); }
    else for (int k = 0; k < size; k++) set_be_byte(r, int(base) + k, pat[k]);
    return r;
}
SPU_INLINE bool op_cbd(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = insert_mask(pref(G(RA(w))) + uint32_t(I7(w)), 1); return true; }
SPU_INLINE bool op_chd(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = insert_mask(pref(G(RA(w))) + uint32_t(I7(w)), 2); return true; }
SPU_INLINE bool op_cwd(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = insert_mask(pref(G(RA(w))) + uint32_t(I7(w)), 4); return true; }
SPU_INLINE bool op_cdd(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = insert_mask(pref(G(RA(w))) + uint32_t(I7(w)), 8); return true; }
SPU_INLINE bool op_cbx(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = insert_mask(pref(G(RA(w))) + pref(G(RB(w))), 1); return true; }
SPU_INLINE bool op_chx(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = insert_mask(pref(G(RA(w))) + pref(G(RB(w))), 2); return true; }
SPU_INLINE bool op_cwx(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = insert_mask(pref(G(RA(w))) + pref(G(RB(w))), 4); return true; }
SPU_INLINE bool op_cdx(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = insert_mask(pref(G(RA(w))) + pref(G(RB(w))), 8); return true; }

SPU_INLINE bool op_ceq(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpeq_epi32(G(RA(w)).vi, G(RB(w)).vi); return true; }
SPU_INLINE bool op_ceqh(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpeq_epi16(G(RA(w)).vi, G(RB(w)).vi); return true; }
SPU_INLINE bool op_ceqb(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpeq_epi8(G(RA(w)).vi, G(RB(w)).vi); return true; }
SPU_INLINE bool op_ceqi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpeq_epi32(G(RA(w)).vi, _mm_set1_epi32(I10(w))); return true; }
SPU_INLINE bool op_ceqhi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpeq_epi16(G(RA(w)).vi, _mm_set1_epi16(static_cast<int16_t>(I10(w)))); return true; }
SPU_INLINE bool op_ceqbi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpeq_epi8(G(RA(w)).vi, _mm_set1_epi8(static_cast<int8_t>(I10(w)))); return true; }
SPU_INLINE bool op_cgt(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpgt_epi32(G(RA(w)).vi, G(RB(w)).vi); return true; }
SPU_INLINE bool op_cgth(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpgt_epi16(G(RA(w)).vi, G(RB(w)).vi); return true; }
SPU_INLINE bool op_cgtb(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpgt_epi8(G(RA(w)).vi, G(RB(w)).vi); return true; }
SPU_INLINE bool op_cgti(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpgt_epi32(G(RA(w)).vi, _mm_set1_epi32(I10(w))); return true; }
SPU_INLINE bool op_cgthi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpgt_epi16(G(RA(w)).vi, _mm_set1_epi16(static_cast<int16_t>(I10(w)))); return true; }
SPU_INLINE bool op_cgtbi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpgt_epi8(G(RA(w)).vi, _mm_set1_epi8(static_cast<int8_t>(I10(w)))); return true; }
SPU_INLINE bool op_clgt(Context& c, Host&, uint32_t w, uint32_t) { __m128i f = _mm_set1_epi32(static_cast<int>(0x80000000u)); G(RT(w)).vi = _mm_cmpgt_epi32(_mm_xor_si128(G(RA(w)).vi, f), _mm_xor_si128(G(RB(w)).vi, f)); return true; }
SPU_INLINE bool op_clgth(Context& c, Host&, uint32_t w, uint32_t) { __m128i f = _mm_set1_epi16(static_cast<short>(0x8000)); G(RT(w)).vi = _mm_cmpgt_epi16(_mm_xor_si128(G(RA(w)).vi, f), _mm_xor_si128(G(RB(w)).vi, f)); return true; }
SPU_INLINE bool op_clgtb(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map8(G(RA(w)), G(RB(w)), [](uint8_t x, uint8_t y) { return uint8_t(x > y ? 0xFF : 0); }); return true; }
SPU_INLINE bool op_clgti(Context& c, Host&, uint32_t w, uint32_t) { __m128i f = _mm_set1_epi32(static_cast<int>(0x80000000u)); G(RT(w)).vi = _mm_cmpgt_epi32(_mm_xor_si128(G(RA(w)).vi, f), _mm_xor_si128(_mm_set1_epi32(I10(w)), f)); return true; }
SPU_INLINE bool op_clgthi(Context& c, Host&, uint32_t w, uint32_t) { __m128i f = _mm_set1_epi16(static_cast<short>(0x8000)); G(RT(w)).vi = _mm_cmpgt_epi16(_mm_xor_si128(G(RA(w)).vi, f), _mm_xor_si128(_mm_set1_epi16(static_cast<short>(I10(w))), f)); return true; }
SPU_INLINE bool op_clgtbi(Context& c, Host&, uint32_t w, uint32_t) { uint8_t i = uint8_t(I10(w)); G(RT(w)) = map8(G(RA(w)), G(RA(w)), [i](uint8_t x, uint8_t) { return uint8_t(x > i ? 0xFF : 0); }); return true; }

SPU_INLINE bool halt_if(Context& c, Host& h, bool cond, uint32_t w, uint32_t pc) {
    if (cond) {
        h.halt(pc, w);
        c.halted = true;
        return false;
    }
    return true;
}
SPU_INLINE bool op_heq(Context& c, Host& h, uint32_t w, uint32_t pc) { return halt_if(c, h, pref(G(RA(w))) == pref(G(RB(w))), w, pc); }
SPU_INLINE bool op_heqi(Context& c, Host& h, uint32_t w, uint32_t pc) { return halt_if(c, h, int32_t(pref(G(RA(w)))) == I10(w), w, pc); }
SPU_INLINE bool op_hgt(Context& c, Host& h, uint32_t w, uint32_t pc) { return halt_if(c, h, int32_t(pref(G(RA(w)))) > int32_t(pref(G(RB(w)))), w, pc); }
SPU_INLINE bool op_hgti(Context& c, Host& h, uint32_t w, uint32_t pc) { return halt_if(c, h, int32_t(pref(G(RA(w)))) > I10(w), w, pc); }
SPU_INLINE bool op_hlgt(Context& c, Host& h, uint32_t w, uint32_t pc) { return halt_if(c, h, pref(G(RA(w))) > pref(G(RB(w))), w, pc); }
SPU_INLINE bool op_hlgti(Context& c, Host& h, uint32_t w, uint32_t pc) { return halt_if(c, h, pref(G(RA(w))) > uint32_t(I10(w)), w, pc); }

template <class F>
SPU_INLINE v128 mapf(const v128& a, const v128& b, F f) { v128 r; for (int i = 0; i < 4; i++) r.f32[i] = f(a.f32[i], b.f32[i]); return r; }

SPU_INLINE double sfx_to_double(uint32_t bits) {
    uint32_t e = (bits >> 23) & 0xFF;
    if (!e) return 0.0;
    uint64_t d = (uint64_t(bits >> 31) << 63) | (uint64_t(e - 127 + 1023) << 52) | (uint64_t(bits & 0x7FFFFF) << 29);
    return std::bit_cast<double>(d);
}

SPU_INLINE uint32_t sfx_from_double(double v) {
    uint64_t d = std::bit_cast<uint64_t>(v);
    uint32_t sign = uint32_t(d >> 63) << 31;
    int32_t e = int32_t((d >> 52) & 0x7FF);
    if (e == 0) return 0;
    if (e == 0x7FF) return sign | 0x7FFFFFFF;
    int32_t se = e - 1023 + 127;
    if (se <= 0) return 0;
    if (se > 255) return sign | 0x7FFFFFFF;
    return sign | (uint32_t(se) << 23) | uint32_t((d >> 29) & 0x7FFFFF);
}

SPU_INLINE __m128i sfx_special(__m128 r) {
    __m128i ri = _mm_castps_si128(r);
    __m128i mag = _mm_and_si128(ri, _mm_set1_epi32(0x7FFFFFFF));
    return _mm_or_si128(_mm_cmpeq_epi32(_mm_and_si128(ri, _mm_set1_epi32(0x7F800000)), _mm_set1_epi32(0x7F800000)), _mm_cmpeq_epi32(mag, _mm_set1_epi32(0x7F7FFFFF)));
}

SPU_INLINE __m128i sfx_exp255(__m128 a) {
    __m128i ai = _mm_castps_si128(a);
    return _mm_cmpeq_epi32(_mm_and_si128(ai, _mm_set1_epi32(0x7F800000)), _mm_set1_epi32(0x7F800000));
}

SPU_INLINE __m128 sfx_pos_zero(__m128 r) {
    __m128i ri = _mm_castps_si128(r);
    __m128i z = _mm_cmpeq_epi32(_mm_and_si128(ri, _mm_set1_epi32(0x7F800000)), _mm_setzero_si128());
    return _mm_castsi128_ps(_mm_andnot_si128(z, ri));
}

template <class F>
SPU_INLINE __m128 sfx_slow(__m128 a, __m128 b, __m128 c, __m128 r, __m128i lanes, F f) {
    alignas(16) uint32_t av[4], bv[4], cv[4], rv[4], lv[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(av), _mm_castps_si128(a));
    _mm_store_si128(reinterpret_cast<__m128i*>(bv), _mm_castps_si128(b));
    _mm_store_si128(reinterpret_cast<__m128i*>(cv), _mm_castps_si128(c));
    _mm_store_si128(reinterpret_cast<__m128i*>(rv), _mm_castps_si128(r));
    _mm_store_si128(reinterpret_cast<__m128i*>(lv), lanes);
    for (int i = 0; i < 4; i++) {
        if (lv[i]) rv[i] = sfx_from_double(f(sfx_to_double(av[i]), sfx_to_double(bv[i]), sfx_to_double(cv[i])));
    }
    return _mm_castsi128_ps(_mm_load_si128(reinterpret_cast<const __m128i*>(rv)));
}

template <class Fast, class Slow>
SPU_INLINE __m128 sfx_op(__m128 a, __m128 b, __m128 c, Fast fast, Slow slow) {
    __m128 r = fast(a, b, c);
    __m128i bad = _mm_or_si128(_mm_or_si128(sfx_special(r), sfx_exp255(a)), _mm_or_si128(sfx_exp255(b), sfx_exp255(c)));
    if (!_mm_testz_si128(bad, bad)) r = sfx_slow(a, b, c, r, bad, slow);
    return sfx_pos_zero(r);
}

SPU_INLINE bool op_fa(Context& c, Host&, uint32_t w, uint32_t) {
    __m128 z = _mm_setzero_ps();
    G(RT(w)).vf = sfx_op(G(RA(w)).vf, G(RB(w)).vf, z, [](__m128 x, __m128 y, __m128) { return _mm_add_ps(x, y); }, [](double x, double y, double) { return x + y; });
    return true;
}
SPU_INLINE bool op_fs(Context& c, Host&, uint32_t w, uint32_t) {
    __m128 z = _mm_setzero_ps();
    G(RT(w)).vf = sfx_op(G(RA(w)).vf, G(RB(w)).vf, z, [](__m128 x, __m128 y, __m128) { return _mm_sub_ps(x, y); }, [](double x, double y, double) { return x - y; });
    return true;
}
SPU_INLINE bool op_fm(Context& c, Host&, uint32_t w, uint32_t) {
    __m128 z = _mm_setzero_ps();
    G(RT(w)).vf = sfx_op(G(RA(w)).vf, G(RB(w)).vf, z, [](__m128 x, __m128 y, __m128) { return _mm_mul_ps(x, y); }, [](double x, double y, double) { return x * y; });
    return true;
}
SPU_INLINE bool op_fma(Context& c, Host&, uint32_t w, uint32_t) {
    G(RRR_T(w)).vf = sfx_op(G(RA(w)).vf, G(RB(w)).vf, G(RRR_C(w)).vf, [](__m128 x, __m128 y, __m128 z) { return _mm_fmadd_ps(x, y, z); }, [](double x, double y, double z) { return std::fma(x, y, z); });
    return true;
}
SPU_INLINE bool op_fms(Context& c, Host&, uint32_t w, uint32_t) {
    G(RRR_T(w)).vf = sfx_op(G(RA(w)).vf, G(RB(w)).vf, G(RRR_C(w)).vf, [](__m128 x, __m128 y, __m128 z) { return _mm_fmsub_ps(x, y, z); }, [](double x, double y, double z) { return std::fma(x, y, -z); });
    return true;
}
SPU_INLINE bool op_fnms(Context& c, Host&, uint32_t w, uint32_t) {
    G(RRR_T(w)).vf = sfx_op(G(RA(w)).vf, G(RB(w)).vf, G(RRR_C(w)).vf, [](__m128 x, __m128 y, __m128 z) { return _mm_fnmadd_ps(x, y, z); }, [](double x, double y, double z) { return std::fma(-x, y, z); });
    return true;
}
SPU_INLINE bool op_frest(Context& c, Host&, uint32_t w, uint32_t) {
    v128 a = G(RA(w)), r;
    for (int i = 0; i < 4; i++) {
        uint32_t bits = a.u32[i];
        if ((bits & 0x7F800000) == 0) r.u32[i] = (bits & 0x80000000) | 0x7FFFFFFF;
        else r.f32[i] = fclamp(1.0f / a.f32[i]);
    }
    G(RT(w)) = r;
    return true;
}
SPU_INLINE bool op_frsqest(Context& c, Host&, uint32_t w, uint32_t) {
    v128 a = G(RA(w)), r;
    for (int i = 0; i < 4; i++) {
        uint32_t bits = a.u32[i] & 0x7FFFFFFF;
        if ((bits & 0x7F800000) == 0) r.u32[i] = 0x7FFFFFFF;
        else r.f32[i] = fclamp(1.0f / std::sqrt(std::bit_cast<float>(bits)));
    }
    G(RT(w)) = r;
    return true;
}
SPU_INLINE bool op_fi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = G(RB(w)); return true; }
SPU_INLINE __m128i sfx_key(__m128i v) {
    __m128i mag = _mm_and_si128(v, _mm_set1_epi32(0x7FFFFFFF));
    __m128i zero = _mm_cmpeq_epi32(_mm_and_si128(v, _mm_set1_epi32(0x7F800000)), _mm_setzero_si128());
    mag = _mm_andnot_si128(zero, mag);
    __m128i neg = _mm_srai_epi32(v, 31);
    return _mm_sub_epi32(_mm_xor_si128(mag, neg), neg);
}
SPU_INLINE __m128i sfx_mag(__m128i v) {
    __m128i mag = _mm_and_si128(v, _mm_set1_epi32(0x7FFFFFFF));
    __m128i zero = _mm_cmpeq_epi32(_mm_and_si128(v, _mm_set1_epi32(0x7F800000)), _mm_setzero_si128());
    return _mm_andnot_si128(zero, mag);
}
SPU_INLINE bool op_fcgt(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpgt_epi32(sfx_key(G(RA(w)).vi), sfx_key(G(RB(w)).vi)); return true; }
SPU_INLINE bool op_fcmgt(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpgt_epi32(sfx_mag(G(RA(w)).vi), sfx_mag(G(RB(w)).vi)); return true; }
SPU_INLINE bool op_fceq(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpeq_epi32(sfx_key(G(RA(w)).vi), sfx_key(G(RB(w)).vi)); return true; }
SPU_INLINE bool op_fcmeq(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpeq_epi32(sfx_mag(G(RA(w)).vi), sfx_mag(G(RB(w)).vi)); return true; }

SPU_INLINE bool op_cflts(Context& c, Host&, uint32_t w, uint32_t) {
    double scale = std::bit_cast<double>(uint64_t(1023 + 173 - int(I8(w))) << 52);
    v128 a = G(RA(w)), r;
    for (int i = 0; i < 4; i++) {
        double v = sfx_to_double(a.u32[i]) * scale;
        r.s32[i] = v >= 2147483647.0 ? INT32_MAX : v <= -2147483648.0 ? INT32_MIN : int32_t(v);
    }
    G(RT(w)) = r;
    return true;
}
SPU_INLINE bool op_cfltu(Context& c, Host&, uint32_t w, uint32_t) {
    double scale = std::bit_cast<double>(uint64_t(1023 + 173 - int(I8(w))) << 52);
    v128 a = G(RA(w)), r;
    for (int i = 0; i < 4; i++) {
        double v = sfx_to_double(a.u32[i]) * scale;
        r.u32[i] = v >= 4294967295.0 ? UINT32_MAX : v <= 0.0 ? 0 : uint32_t(v);
    }
    G(RT(w)) = r;
    return true;
}
SPU_INLINE bool op_csflt(Context& c, Host&, uint32_t w, uint32_t) {
    double scale = std::bit_cast<double>(uint64_t(1023 + int(I8(w)) - 155) << 52);
    v128 a = G(RA(w)), r;
    for (int i = 0; i < 4; i++) r.u32[i] = sfx_from_double(double(a.s32[i]) * scale);
    G(RT(w)) = r;
    return true;
}
SPU_INLINE bool op_cuflt(Context& c, Host&, uint32_t w, uint32_t) {
    double scale = std::bit_cast<double>(uint64_t(1023 + int(I8(w)) - 155) << 52);
    v128 a = G(RA(w)), r;
    for (int i = 0; i < 4; i++) r.u32[i] = sfx_from_double(double(a.u32[i]) * scale);
    G(RT(w)) = r;
    return true;
}
SPU_INLINE bool op_fesd(Context& c, Host&, uint32_t w, uint32_t) { RoundGuard g; v128 a = G(RA(w)), r; r.u64[1] = std::bit_cast<uint64_t>(double(a.f32[3])); r.u64[0] = std::bit_cast<uint64_t>(double(a.f32[1])); G(RT(w)) = r; return true; }
SPU_INLINE bool op_frds(Context& c, Host&, uint32_t w, uint32_t) { RoundGuard g; v128 a = G(RA(w)), r{}; r.f32[3] = float(std::bit_cast<double>(a.u64[1])); r.f32[1] = float(std::bit_cast<double>(a.u64[0])); G(RT(w)) = r; return true; }

template <class F>
SPU_INLINE v128 mapd(const v128& a, const v128& b, F f) { RoundGuard g; v128 r; for (int i = 0; i < 2; i++) r.u64[i] = std::bit_cast<uint64_t>(f(std::bit_cast<double>(a.u64[i]), std::bit_cast<double>(b.u64[i]))); return r; }
SPU_INLINE bool op_dfa(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = mapd(G(RA(w)), G(RB(w)), [](double x, double y) { return x + y; }); return true; }
SPU_INLINE bool op_dfs(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = mapd(G(RA(w)), G(RB(w)), [](double x, double y) { return x - y; }); return true; }
SPU_INLINE bool op_dfm(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = mapd(G(RA(w)), G(RB(w)), [](double x, double y) { return x * y; }); return true; }
SPU_INLINE bool dfm_family(Context& c, uint32_t w, int kind) {
    RoundGuard g;
    v128 a = G(RA(w)), b = G(RB(w)), t = G(RT(w)), r;
    for (int i = 0; i < 2; i++) {
        double x = std::bit_cast<double>(a.u64[i]), y = std::bit_cast<double>(b.u64[i]), z = std::bit_cast<double>(t.u64[i]);
        double v = kind == 0 ? std::fma(x, y, z) : kind == 1 ? std::fma(x, y, -z) : kind == 2 ? -std::fma(x, y, -z) : -std::fma(x, y, z);
        r.u64[i] = std::bit_cast<uint64_t>(v);
    }
    G(RT(w)) = r;
    return true;
}
SPU_INLINE bool op_dfma(Context& c, Host&, uint32_t w, uint32_t) { return dfm_family(c, w, 0); }
SPU_INLINE bool op_dfms(Context& c, Host&, uint32_t w, uint32_t) { return dfm_family(c, w, 1); }
SPU_INLINE bool op_dfnms(Context& c, Host&, uint32_t w, uint32_t) { return dfm_family(c, w, 2); }
SPU_INLINE bool op_dfnma(Context& c, Host&, uint32_t w, uint32_t) { return dfm_family(c, w, 3); }
SPU_INLINE bool dcmp(Context& c, uint32_t w, int kind) {
    v128 a = G(RA(w)), b = G(RB(w)), r;
    for (int i = 0; i < 2; i++) {
        double x = std::bit_cast<double>(a.u64[i]), y = std::bit_cast<double>(b.u64[i]);
        if (kind & 2) { x = std::fabs(x); y = std::fabs(y); }
        bool v = (kind & 1) ? x == y : x > y;
        r.u64[i] = v ? ~0ull : 0;
    }
    G(RT(w)) = r;
    return true;
}
SPU_INLINE bool op_dfcgt(Context& c, Host&, uint32_t w, uint32_t) { return dcmp(c, w, 0); }
SPU_INLINE bool op_dfceq(Context& c, Host&, uint32_t w, uint32_t) { return dcmp(c, w, 1); }
SPU_INLINE bool op_dfcmgt(Context& c, Host&, uint32_t w, uint32_t) { return dcmp(c, w, 2); }
SPU_INLINE bool op_dfcmeq(Context& c, Host&, uint32_t w, uint32_t) { return dcmp(c, w, 3); }
SPU_INLINE bool op_dftsv(Context& c, Host&, uint32_t w, uint32_t) {
    uint32_t m = I7(w) & 0x7F;
    v128 a = G(RA(w)), r;
    for (int i = 0; i < 2; i++) {
        double x = std::bit_cast<double>(a.u64[i]);
        bool neg = std::signbit(x);
        bool v = ((m & 0x40) && std::isnan(x)) || ((m & 0x20) && std::isinf(x) && !neg) || ((m & 0x10) && std::isinf(x) && neg) ||
                 ((m & 0x08) && x == 0 && !neg) || ((m & 0x04) && x == 0 && neg) || ((m & 0x02) && std::fpclassify(x) == FP_SUBNORMAL && !neg) ||
                 ((m & 0x01) && std::fpclassify(x) == FP_SUBNORMAL && neg);
        r.u64[i] = v ? ~0ull : 0;
    }
    G(RT(w)) = r;
    return true;
}
SPU_INLINE bool op_fscrrd(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = v128{}; return true; }
SPU_INLINE bool op_fscrwr(Context&, Host&, uint32_t, uint32_t) { return true; }

#undef G

}

}

#pragma clang diagnostic pop
