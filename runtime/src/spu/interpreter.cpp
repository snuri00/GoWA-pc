#include "spu/context.h"
#include "ppu/ops.h"

#include <bit>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <limits>

namespace spu {

namespace {

using Handler = bool (*)(Context&, Host&, uint32_t, uint32_t);

inline int32_t sext(uint32_t v, int bits) { return static_cast<int32_t>(v << (32 - bits)) >> (32 - bits); }
inline uint32_t RT(uint32_t w) { return w & 0x7F; }
inline uint32_t RA(uint32_t w) { return (w >> 7) & 0x7F; }
inline uint32_t RB(uint32_t w) { return (w >> 14) & 0x7F; }
inline uint32_t RRR_T(uint32_t w) { return (w >> 21) & 0x7F; }
inline uint32_t RRR_C(uint32_t w) { return w & 0x7F; }
inline int32_t I7(uint32_t w) { return sext((w >> 14) & 0x7F, 7); }
inline uint32_t I8(uint32_t w) { return (w >> 14) & 0xFF; }
inline int32_t I10(uint32_t w) { return sext((w >> 14) & 0x3FF, 10); }
inline uint32_t I16(uint32_t w) { return (w >> 7) & 0xFFFF; }
inline int32_t S16(uint32_t w) { return sext((w >> 7) & 0xFFFF, 16); }
inline uint32_t I18(uint32_t w) { return (w >> 7) & 0x3FFFF; }

inline v128 load_ls(const Context& c, uint32_t addr) {
    v128 r;
    r.vi = ppu::bswap128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(c.ls + (addr & LS_MASK))));
    return r;
}

inline void store_ls(Context& c, uint32_t addr, const v128& v) {
    _mm_storeu_si128(reinterpret_cast<__m128i*>(c.ls + (addr & LS_MASK)), ppu::bswap128(v.vi));
}

inline v128 splat32(uint32_t v) { v128 r; r.vi = _mm_set1_epi32(static_cast<int32_t>(v)); return r; }
inline v128 splat16(uint16_t v) { v128 r; r.vi = _mm_set1_epi16(static_cast<int16_t>(v)); return r; }
inline v128 splat8(uint8_t v) { v128 r; r.vi = _mm_set1_epi8(static_cast<int8_t>(v)); return r; }
inline v128 pref_only(uint32_t v) { v128 r{}; r.u32[3] = v; return r; }

inline uint8_t be_byte(const v128& v, int i) { return v.u8[15 - i]; }
inline void set_be_byte(v128& v, int i, uint8_t b) { v.u8[15 - i] = b; }

#define G(n) c.gpr[n]

template <class F>
inline v128 map32(const v128& a, const v128& b, F f) { v128 r; for (int i = 0; i < 4; i++) r.u32[i] = f(a.u32[i], b.u32[i]); return r; }
template <class F>
inline v128 map16(const v128& a, const v128& b, F f) { v128 r; for (int i = 0; i < 8; i++) r.u16[i] = f(a.u16[i], b.u16[i]); return r; }
template <class F>
inline v128 map8(const v128& a, const v128& b, F f) { v128 r; for (int i = 0; i < 16; i++) r.u8[i] = f(a.u8[i], b.u8[i]); return r; }

struct RoundGuard {
    unsigned old;
    RoundGuard() : old(_mm_getcsr()) { _mm_setcsr((old & ~0x6000u) & ~0x8040u); }
    ~RoundGuard() { _mm_setcsr(old); }
};

inline float fzero_mask(float r, float a, float b) {
    return (a == 0.0f || b == 0.0f) ? 0.0f * std::copysign(1.0f, a) * std::copysign(1.0f, b) : r;
}

inline uint32_t smax_fix(uint32_t bits) {
    uint32_t e = bits & 0x7F800000;
    if (e == 0x7F800000) return (bits & 0x80000000) | 0x7FFFFFFF;
    return bits;
}

inline float fclamp(float x) { return std::bit_cast<float>(smax_fix(std::bit_cast<uint32_t>(x))); }

bool op_stop(Context& c, Host& h, uint32_t w, uint32_t pc) {
    c.pc = pc + 4;
    return h.stop_and_signal(w & 0x3FFF);
}
bool op_nop(Context&, Host&, uint32_t, uint32_t) { return true; }
bool op_rdch(Context& c, Host& h, uint32_t w, uint32_t pc) {
    uint32_t v = h.read_channel(RA(w));
    G(RT(w)) = pref_only(v);
    return !c.halted;
}
bool op_rchcnt(Context& c, Host& h, uint32_t w, uint32_t) { G(RT(w)) = pref_only(h.channel_count(RA(w))); return true; }
bool op_wrch(Context& c, Host& h, uint32_t w, uint32_t) { h.write_channel(RA(w), pref(G(RT(w)))); return !c.halted; }
bool op_mfspr(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = v128{}; return true; }
bool op_illegal(Context& c, Host& h, uint32_t w, uint32_t pc) { h.halt(pc, w); c.halted = true; return false; }

bool op_lqd(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = load_ls(c, pref(G(RA(w))) + (I10(w) << 4)); return true; }
bool op_lqx(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = load_ls(c, pref(G(RA(w))) + pref(G(RB(w)))); return true; }
bool op_lqa(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = load_ls(c, static_cast<uint32_t>(S16(w) << 2)); return true; }
bool op_lqr(Context& c, Host&, uint32_t w, uint32_t pc) { G(RT(w)) = load_ls(c, pc + (S16(w) << 2)); return true; }
bool op_stqd(Context& c, Host&, uint32_t w, uint32_t) { store_ls(c, pref(G(RA(w))) + (I10(w) << 4), G(RT(w))); return true; }
bool op_stqx(Context& c, Host&, uint32_t w, uint32_t) { store_ls(c, pref(G(RA(w))) + pref(G(RB(w))), G(RT(w))); return true; }
bool op_stqa(Context& c, Host&, uint32_t w, uint32_t) { store_ls(c, static_cast<uint32_t>(S16(w) << 2), G(RT(w))); return true; }
bool op_stqr(Context& c, Host&, uint32_t w, uint32_t pc) { store_ls(c, pc + (S16(w) << 2), G(RT(w))); return true; }

bool op_br(Context& c, Host&, uint32_t w, uint32_t pc) { c.pc = (pc + (S16(w) << 2)) & PC_MASK; return true; }
bool op_bra(Context& c, Host&, uint32_t w, uint32_t) { c.pc = static_cast<uint32_t>(S16(w) << 2) & PC_MASK; return true; }
bool op_brsl(Context& c, Host&, uint32_t w, uint32_t pc) { uint32_t t = (pc + (S16(w) << 2)) & PC_MASK; G(RT(w)) = pref_only((pc + 4) & PC_MASK); c.pc = t; return true; }
bool op_brasl(Context& c, Host&, uint32_t w, uint32_t pc) { uint32_t t = static_cast<uint32_t>(S16(w) << 2) & PC_MASK; G(RT(w)) = pref_only((pc + 4) & PC_MASK); c.pc = t; return true; }
bool op_brz(Context& c, Host&, uint32_t w, uint32_t pc) { if (pref(G(RT(w))) == 0) c.pc = (pc + (S16(w) << 2)) & PC_MASK; return true; }
bool op_brnz(Context& c, Host&, uint32_t w, uint32_t pc) { if (pref(G(RT(w))) != 0) c.pc = (pc + (S16(w) << 2)) & PC_MASK; return true; }
bool op_brhz(Context& c, Host&, uint32_t w, uint32_t pc) { if (G(RT(w)).u16[6] == 0) c.pc = (pc + (S16(w) << 2)) & PC_MASK; return true; }
bool op_brhnz(Context& c, Host&, uint32_t w, uint32_t pc) { if (G(RT(w)).u16[6] != 0) c.pc = (pc + (S16(w) << 2)) & PC_MASK; return true; }

inline void interrupt_bits(Context& c, uint32_t w) {
    if (w & 0x80000) c.interrupts_enabled = true;
    if (w & 0x100000) c.interrupts_enabled = false;
}

bool op_bi(Context& c, Host&, uint32_t w, uint32_t) { c.pc = pref(G(RA(w))) & PC_MASK; interrupt_bits(c, w); return true; }
bool op_bisl(Context& c, Host&, uint32_t w, uint32_t pc) { uint32_t t = pref(G(RA(w))) & PC_MASK; G(RT(w)) = pref_only((pc + 4) & PC_MASK); c.pc = t; interrupt_bits(c, w); return true; }
bool op_iret(Context& c, Host&, uint32_t w, uint32_t) { c.pc = c.srr0 & PC_MASK; interrupt_bits(c, w); return true; }
bool op_bisled(Context& c, Host& h, uint32_t w, uint32_t pc) {
    uint32_t t = pref(G(RA(w))) & PC_MASK;
    G(RT(w)) = pref_only((pc + 4) & PC_MASK);
    if (h.channel_count(0)) c.pc = t;
    interrupt_bits(c, w);
    return true;
}
bool op_biz(Context& c, Host&, uint32_t w, uint32_t) { if (pref(G(RT(w))) == 0) { c.pc = pref(G(RA(w))) & PC_MASK; interrupt_bits(c, w); } return true; }
bool op_binz(Context& c, Host&, uint32_t w, uint32_t) { if (pref(G(RT(w))) != 0) { c.pc = pref(G(RA(w))) & PC_MASK; interrupt_bits(c, w); } return true; }
bool op_bihz(Context& c, Host&, uint32_t w, uint32_t) { if (G(RT(w)).u16[6] == 0) { c.pc = pref(G(RA(w))) & PC_MASK; interrupt_bits(c, w); } return true; }
bool op_bihnz(Context& c, Host&, uint32_t w, uint32_t) { if (G(RT(w)).u16[6] != 0) { c.pc = pref(G(RA(w))) & PC_MASK; interrupt_bits(c, w); } return true; }

bool op_il(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = splat32(static_cast<uint32_t>(S16(w))); return true; }
bool op_ilh(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = splat16(static_cast<uint16_t>(I16(w))); return true; }
bool op_ilhu(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = splat32(I16(w) << 16); return true; }
bool op_ila(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = splat32(I18(w)); return true; }
bool op_iohl(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_or_si128(G(RT(w)).vi, _mm_set1_epi32(static_cast<int32_t>(I16(w)))); return true; }
bool op_fsmbi(Context& c, Host&, uint32_t w, uint32_t) {
    uint32_t m = I16(w);
    v128 r;
    for (int i = 0; i < 16; i++) set_be_byte(r, i, (m >> (15 - i)) & 1 ? 0xFF : 0);
    G(RT(w)) = r;
    return true;
}

bool op_a(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_add_epi32(G(RA(w)).vi, G(RB(w)).vi); return true; }
bool op_ah(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_add_epi16(G(RA(w)).vi, G(RB(w)).vi); return true; }
bool op_ai(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_add_epi32(G(RA(w)).vi, _mm_set1_epi32(I10(w))); return true; }
bool op_ahi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_add_epi16(G(RA(w)).vi, _mm_set1_epi16(static_cast<int16_t>(I10(w)))); return true; }
bool op_sf(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_sub_epi32(G(RB(w)).vi, G(RA(w)).vi); return true; }
bool op_sfh(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_sub_epi16(G(RB(w)).vi, G(RA(w)).vi); return true; }
bool op_sfi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_sub_epi32(_mm_set1_epi32(I10(w)), G(RA(w)).vi); return true; }
bool op_sfhi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_sub_epi16(_mm_set1_epi16(static_cast<int16_t>(I10(w))), G(RA(w)).vi); return true; }
bool op_addx(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map32(G(RA(w)), G(RB(w)), [&, i = 0](uint32_t x, uint32_t y) mutable { return x + y + (G(RT(w)).u32[i++] & 1); }); return true; }
bool op_sfx(Context& c, Host&, uint32_t w, uint32_t) { v128 t = G(RT(w)); G(RT(w)) = map32(G(RA(w)), G(RB(w)), [&, i = 0](uint32_t x, uint32_t y) mutable { return y - x - (1 - (t.u32[i++] & 1)); }); return true; }
bool op_cg(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map32(G(RA(w)), G(RB(w)), [](uint32_t x, uint32_t y) { return uint32_t(uint64_t(x) + y > 0xFFFFFFFFull); }); return true; }
bool op_bg(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map32(G(RA(w)), G(RB(w)), [](uint32_t x, uint32_t y) { return uint32_t(y >= x); }); return true; }
bool op_cgx(Context& c, Host&, uint32_t w, uint32_t) { v128 t = G(RT(w)); G(RT(w)) = map32(G(RA(w)), G(RB(w)), [&, i = 0](uint32_t x, uint32_t y) mutable { return uint32_t(uint64_t(x) + y + (t.u32[i++] & 1) > 0xFFFFFFFFull); }); return true; }
bool op_bgx(Context& c, Host&, uint32_t w, uint32_t) { v128 t = G(RT(w)); G(RT(w)) = map32(G(RA(w)), G(RB(w)), [&, i = 0](uint32_t x, uint32_t y) mutable { int64_t r = int64_t(y) - int64_t(x) - (1 - (t.u32[i++] & 1)); return uint32_t(r >= 0); }); return true; }

bool op_mpy(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map32(G(RA(w)), G(RB(w)), [](uint32_t x, uint32_t y) { return uint32_t(int32_t(int16_t(x)) * int16_t(y)); }); return true; }
bool op_mpyu(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map32(G(RA(w)), G(RB(w)), [](uint32_t x, uint32_t y) { return uint32_t(uint16_t(x)) * uint16_t(y); }); return true; }
bool op_mpyi(Context& c, Host&, uint32_t w, uint32_t) { int32_t imm = I10(w); G(RT(w)) = map32(G(RA(w)), G(RA(w)), [imm](uint32_t x, uint32_t) { return uint32_t(int32_t(int16_t(x)) * int32_t(int16_t(imm))); }); return true; }
bool op_mpyui(Context& c, Host&, uint32_t w, uint32_t) { uint32_t imm = uint32_t(I10(w)) & 0xFFFF; G(RT(w)) = map32(G(RA(w)), G(RA(w)), [imm](uint32_t x, uint32_t) { return uint32_t(uint16_t(x)) * imm; }); return true; }
bool op_mpya(Context& c, Host&, uint32_t w, uint32_t) { v128 a = G(RA(w)), b = G(RB(w)), cc = G(RRR_C(w)); v128 r; for (int i = 0; i < 4; i++) r.u32[i] = uint32_t(int32_t(int16_t(a.u32[i])) * int16_t(b.u32[i])) + cc.u32[i]; G(RRR_T(w)) = r; return true; }
bool op_mpyh(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map32(G(RA(w)), G(RB(w)), [](uint32_t x, uint32_t y) { return ((x >> 16) * (y & 0xFFFF)) << 16; }); return true; }
bool op_mpys(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map32(G(RA(w)), G(RB(w)), [](uint32_t x, uint32_t y) { return uint32_t((int32_t(int16_t(x)) * int16_t(y)) >> 16); }); return true; }
bool op_mpyhh(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map32(G(RA(w)), G(RB(w)), [](uint32_t x, uint32_t y) { return uint32_t(int32_t(int16_t(x >> 16)) * int16_t(y >> 16)); }); return true; }
bool op_mpyhhu(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map32(G(RA(w)), G(RB(w)), [](uint32_t x, uint32_t y) { return (x >> 16) * (y >> 16); }); return true; }
bool op_mpyhha(Context& c, Host&, uint32_t w, uint32_t) { v128 t = G(RT(w)); G(RT(w)) = map32(G(RA(w)), G(RB(w)), [&, i = 0](uint32_t x, uint32_t y) mutable { return uint32_t(int32_t(int16_t(x >> 16)) * int16_t(y >> 16)) + t.u32[i++]; }); return true; }
bool op_mpyhhau(Context& c, Host&, uint32_t w, uint32_t) { v128 t = G(RT(w)); G(RT(w)) = map32(G(RA(w)), G(RB(w)), [&, i = 0](uint32_t x, uint32_t y) mutable { return (x >> 16) * (y >> 16) + t.u32[i++]; }); return true; }

bool op_clz(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map32(G(RA(w)), G(RA(w)), [](uint32_t x, uint32_t) { return uint32_t(std::countl_zero(x)); }); return true; }
bool op_cntb(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map8(G(RA(w)), G(RA(w)), [](uint8_t x, uint8_t) { return uint8_t(std::popcount(x)); }); return true; }
bool op_avgb(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_avg_epu8(G(RA(w)).vi, G(RB(w)).vi); return true; }
bool op_absdb(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map8(G(RA(w)), G(RB(w)), [](uint8_t x, uint8_t y) { return uint8_t(x > y ? x - y : y - x); }); return true; }
bool op_sumb(Context& c, Host&, uint32_t w, uint32_t) {
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
bool op_xsbh(Context& c, Host&, uint32_t w, uint32_t) { v128 a = G(RA(w)), r; for (int i = 0; i < 8; i++) r.s16[i] = int8_t(a.u16[i]); G(RT(w)) = r; return true; }
bool op_xshw(Context& c, Host&, uint32_t w, uint32_t) { v128 a = G(RA(w)), r; for (int i = 0; i < 4; i++) r.s32[i] = int16_t(a.u32[i]); G(RT(w)) = r; return true; }
bool op_xswd(Context& c, Host&, uint32_t w, uint32_t) { v128 a = G(RA(w)), r; for (int i = 0; i < 2; i++) r.u64[i] = uint64_t(int64_t(int32_t(a.u64[i]))); G(RT(w)) = r; return true; }

bool op_and(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_and_si128(G(RA(w)).vi, G(RB(w)).vi); return true; }
bool op_andc(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_andnot_si128(G(RB(w)).vi, G(RA(w)).vi); return true; }
bool op_or(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_or_si128(G(RA(w)).vi, G(RB(w)).vi); return true; }
bool op_orc(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_or_si128(G(RA(w)).vi, _mm_xor_si128(G(RB(w)).vi, _mm_set1_epi32(-1))); return true; }
bool op_xor(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_xor_si128(G(RA(w)).vi, G(RB(w)).vi); return true; }
bool op_nand(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_xor_si128(_mm_and_si128(G(RA(w)).vi, G(RB(w)).vi), _mm_set1_epi32(-1)); return true; }
bool op_nor(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_xor_si128(_mm_or_si128(G(RA(w)).vi, G(RB(w)).vi), _mm_set1_epi32(-1)); return true; }
bool op_eqv(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_xor_si128(_mm_xor_si128(G(RA(w)).vi, G(RB(w)).vi), _mm_set1_epi32(-1)); return true; }
bool op_andi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_and_si128(G(RA(w)).vi, _mm_set1_epi32(I10(w))); return true; }
bool op_andhi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_and_si128(G(RA(w)).vi, _mm_set1_epi16(static_cast<int16_t>(I10(w)))); return true; }
bool op_andbi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_and_si128(G(RA(w)).vi, _mm_set1_epi8(static_cast<int8_t>(I10(w)))); return true; }
bool op_ori(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_or_si128(G(RA(w)).vi, _mm_set1_epi32(I10(w))); return true; }
bool op_orhi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_or_si128(G(RA(w)).vi, _mm_set1_epi16(static_cast<int16_t>(I10(w)))); return true; }
bool op_orbi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_or_si128(G(RA(w)).vi, _mm_set1_epi8(static_cast<int8_t>(I10(w)))); return true; }
bool op_xori(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_xor_si128(G(RA(w)).vi, _mm_set1_epi32(I10(w))); return true; }
bool op_xorhi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_xor_si128(G(RA(w)).vi, _mm_set1_epi16(static_cast<int16_t>(I10(w)))); return true; }
bool op_xorbi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_xor_si128(G(RA(w)).vi, _mm_set1_epi8(static_cast<int8_t>(I10(w)))); return true; }
bool op_orx(Context& c, Host&, uint32_t w, uint32_t) { v128 a = G(RA(w)); G(RT(w)) = pref_only(a.u32[0] | a.u32[1] | a.u32[2] | a.u32[3]); return true; }
bool op_selb(Context& c, Host&, uint32_t w, uint32_t) {
    v128 m = G(RRR_C(w));
    G(RRR_T(w)).vi = _mm_or_si128(_mm_andnot_si128(m.vi, G(RA(w)).vi), _mm_and_si128(m.vi, G(RB(w)).vi));
    return true;
}
bool op_shufb(Context& c, Host&, uint32_t w, uint32_t) {
    v128 a = G(RA(w)), b = G(RB(w)), m = G(RRR_C(w)), r;
    for (int i = 0; i < 16; i++) {
        uint8_t s = be_byte(m, i);
        uint8_t v;
        if ((s & 0xC0) == 0x80) v = 0x00;
        else if ((s & 0xE0) == 0xC0) v = 0xFF;
        else if ((s & 0xE0) == 0xE0) v = 0x80;
        else {
            uint32_t idx = s & 0x1F;
            v = idx < 16 ? be_byte(a, idx) : be_byte(b, idx - 16);
        }
        set_be_byte(r, i, v);
    }
    G(RRR_T(w)) = r;
    return true;
}

bool op_fsmb(Context& c, Host&, uint32_t w, uint32_t) { uint32_t m = pref(G(RA(w))) & 0xFFFF; v128 r; for (int i = 0; i < 16; i++) set_be_byte(r, i, (m >> (15 - i)) & 1 ? 0xFF : 0); G(RT(w)) = r; return true; }
bool op_fsmh(Context& c, Host&, uint32_t w, uint32_t) { uint32_t m = pref(G(RA(w))) & 0xFF; v128 r; for (int i = 0; i < 8; i++) r.u16[7 - i] = (m >> (7 - i)) & 1 ? 0xFFFF : 0; G(RT(w)) = r; return true; }
bool op_fsm(Context& c, Host&, uint32_t w, uint32_t) { uint32_t m = pref(G(RA(w))) & 0xF; v128 r; for (int i = 0; i < 4; i++) r.u32[3 - i] = (m >> (3 - i)) & 1 ? 0xFFFFFFFF : 0; G(RT(w)) = r; return true; }
bool op_gbb(Context& c, Host&, uint32_t w, uint32_t) { v128 a = G(RA(w)); uint32_t m = 0; for (int i = 0; i < 16; i++) m = (m << 1) | (be_byte(a, i) & 1); G(RT(w)) = pref_only(m); return true; }
bool op_gbh(Context& c, Host&, uint32_t w, uint32_t) { v128 a = G(RA(w)); uint32_t m = 0; for (int i = 0; i < 8; i++) m = (m << 1) | (a.u16[7 - i] & 1); G(RT(w)) = pref_only(m); return true; }
bool op_gb(Context& c, Host&, uint32_t w, uint32_t) { v128 a = G(RA(w)); uint32_t m = 0; for (int i = 0; i < 4; i++) m = (m << 1) | (a.u32[3 - i] & 1); G(RT(w)) = pref_only(m); return true; }

bool op_shlh(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map16(G(RA(w)), G(RB(w)), [](uint16_t x, uint16_t y) { uint32_t s = y & 0x1F; return uint16_t(s > 15 ? 0 : x << s); }); return true; }
bool op_shlhi(Context& c, Host&, uint32_t w, uint32_t) { uint32_t s = uint32_t(I7(w)) & 0x1F; G(RT(w)) = map16(G(RA(w)), G(RA(w)), [s](uint16_t x, uint16_t) { return uint16_t(s > 15 ? 0 : x << s); }); return true; }
bool op_shl(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map32(G(RA(w)), G(RB(w)), [](uint32_t x, uint32_t y) { uint32_t s = y & 0x3F; return s > 31 ? 0u : x << s; }); return true; }
bool op_shli(Context& c, Host&, uint32_t w, uint32_t) { uint32_t s = uint32_t(I7(w)) & 0x3F; G(RT(w)) = map32(G(RA(w)), G(RA(w)), [s](uint32_t x, uint32_t) { return s > 31 ? 0u : x << s; }); return true; }
bool op_roth(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map16(G(RA(w)), G(RB(w)), [](uint16_t x, uint16_t y) { return std::rotl(x, y & 0xF); }); return true; }
bool op_rothi(Context& c, Host&, uint32_t w, uint32_t) { int s = I7(w) & 0xF; G(RT(w)) = map16(G(RA(w)), G(RA(w)), [s](uint16_t x, uint16_t) { return std::rotl(x, s); }); return true; }
bool op_rot(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map32(G(RA(w)), G(RB(w)), [](uint32_t x, uint32_t y) { return std::rotl(x, int(y & 0x1F)); }); return true; }
bool op_roti(Context& c, Host&, uint32_t w, uint32_t) { int s = I7(w) & 0x1F; G(RT(w)) = map32(G(RA(w)), G(RA(w)), [s](uint32_t x, uint32_t) { return std::rotl(x, s); }); return true; }
bool op_rothm(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map16(G(RA(w)), G(RB(w)), [](uint16_t x, uint16_t y) { uint32_t s = (0u - y) & 0x1F; return uint16_t(s > 15 ? 0 : x >> s); }); return true; }
bool op_rothmi(Context& c, Host&, uint32_t w, uint32_t) { uint32_t s = (0u - uint32_t(I7(w))) & 0x1F; G(RT(w)) = map16(G(RA(w)), G(RA(w)), [s](uint16_t x, uint16_t) { return uint16_t(s > 15 ? 0 : x >> s); }); return true; }
bool op_rotm(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map32(G(RA(w)), G(RB(w)), [](uint32_t x, uint32_t y) { uint32_t s = (0u - y) & 0x3F; return s > 31 ? 0u : x >> s; }); return true; }
bool op_rotmi(Context& c, Host&, uint32_t w, uint32_t) { uint32_t s = (0u - uint32_t(I7(w))) & 0x3F; G(RT(w)) = map32(G(RA(w)), G(RA(w)), [s](uint32_t x, uint32_t) { return s > 31 ? 0u : x >> s; }); return true; }
bool op_rotmah(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map16(G(RA(w)), G(RB(w)), [](uint16_t x, uint16_t y) { uint32_t s = (0u - y) & 0x1F; return uint16_t(int16_t(x) >> (s > 15 ? 15 : s)); }); return true; }
bool op_rotmahi(Context& c, Host&, uint32_t w, uint32_t) { uint32_t s = (0u - uint32_t(I7(w))) & 0x1F; G(RT(w)) = map16(G(RA(w)), G(RA(w)), [s](uint16_t x, uint16_t) { return uint16_t(int16_t(x) >> (s > 15 ? 15 : s)); }); return true; }
bool op_rotma(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map32(G(RA(w)), G(RB(w)), [](uint32_t x, uint32_t y) { uint32_t s = (0u - y) & 0x3F; return uint32_t(int32_t(x) >> (s > 31 ? 31 : s)); }); return true; }
bool op_rotmai(Context& c, Host&, uint32_t w, uint32_t) { uint32_t s = (0u - uint32_t(I7(w))) & 0x3F; G(RT(w)) = map32(G(RA(w)), G(RA(w)), [s](uint32_t x, uint32_t) { return uint32_t(int32_t(x) >> (s > 31 ? 31 : s)); }); return true; }

inline v128 qshl_bytes(const v128& a, uint32_t n) { v128 r{}; for (int i = 0; i < 16; i++) { int j = i - int(n); r.u8[i] = (j >= 0 && n < 16) ? a.u8[j] : 0; } return r; }
inline v128 qshr_bytes(const v128& a, uint32_t n) { v128 r{}; for (int i = 0; i < 16; i++) { int j = i + int(n); r.u8[i] = (j < 16 && n < 16) ? a.u8[j] : 0; } return r; }
inline v128 qrot_bytes(const v128& a, uint32_t n) { v128 r; for (int i = 0; i < 16; i++) r.u8[i] = a.u8[(i - int(n)) & 15]; return r; }
inline v128 qshl_bits(const v128& a, uint32_t n) { if (!n) return a; v128 r; r.u64[1] = (a.u64[1] << n) | (a.u64[0] >> (64 - n)); r.u64[0] = a.u64[0] << n; return r; }
inline v128 qshr_bits(const v128& a, uint32_t n) { if (!n) return a; v128 r; r.u64[0] = (a.u64[0] >> n) | (a.u64[1] << (64 - n)); r.u64[1] = a.u64[1] >> n; return r; }
inline v128 qrot_bits(const v128& a, uint32_t n) { if (!n) return a; v128 r; r.u64[1] = (a.u64[1] << n) | (a.u64[0] >> (64 - n)); r.u64[0] = (a.u64[0] << n) | (a.u64[1] >> (64 - n)); return r; }

bool op_shlqbi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qshl_bits(G(RA(w)), pref(G(RB(w))) & 7); return true; }
bool op_shlqbii(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qshl_bits(G(RA(w)), uint32_t(I7(w)) & 7); return true; }
bool op_shlqby(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qshl_bytes(G(RA(w)), pref(G(RB(w))) & 0x1F); return true; }
bool op_shlqbyi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qshl_bytes(G(RA(w)), uint32_t(I7(w)) & 0x1F); return true; }
bool op_shlqbybi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qshl_bytes(G(RA(w)), (pref(G(RB(w))) >> 3) & 0x1F); return true; }
bool op_rotqby(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qrot_bytes(G(RA(w)), pref(G(RB(w))) & 0xF); return true; }
bool op_rotqbyi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qrot_bytes(G(RA(w)), uint32_t(I7(w)) & 0xF); return true; }
bool op_rotqbybi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qrot_bytes(G(RA(w)), (pref(G(RB(w))) >> 3) & 0xF); return true; }
bool op_rotqbi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qrot_bits(G(RA(w)), pref(G(RB(w))) & 7); return true; }
bool op_rotqbii(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qrot_bits(G(RA(w)), uint32_t(I7(w)) & 7); return true; }
bool op_rotqmby(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qshr_bytes(G(RA(w)), (0u - pref(G(RB(w)))) & 0x1F); return true; }
bool op_rotqmbyi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qshr_bytes(G(RA(w)), (0u - uint32_t(I7(w))) & 0x1F); return true; }
bool op_rotqmbybi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qshr_bytes(G(RA(w)), (0u - (pref(G(RB(w))) >> 3)) & 0x1F); return true; }
bool op_rotqmbi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qshr_bits(G(RA(w)), (0u - pref(G(RB(w)))) & 7); return true; }
bool op_rotqmbii(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = qshr_bits(G(RA(w)), (0u - uint32_t(I7(w))) & 7); return true; }

inline v128 insert_mask(uint32_t t, int size) {
    v128 r;
    for (int i = 0; i < 16; i++) set_be_byte(r, i, uint8_t(0x10 + i));
    uint32_t base = t & ~uint32_t(size - 1) & 0xF;
    static const uint8_t pat[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    if (size == 1) set_be_byte(r, int(t & 0xF), 0x03);
    else if (size == 2) { set_be_byte(r, int(base), 0x02); set_be_byte(r, int(base) + 1, 0x03); }
    else for (int k = 0; k < size; k++) set_be_byte(r, int(base) + k, pat[k]);
    return r;
}
bool op_cbd(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = insert_mask(pref(G(RA(w))) + uint32_t(I7(w)), 1); return true; }
bool op_chd(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = insert_mask(pref(G(RA(w))) + uint32_t(I7(w)), 2); return true; }
bool op_cwd(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = insert_mask(pref(G(RA(w))) + uint32_t(I7(w)), 4); return true; }
bool op_cdd(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = insert_mask(pref(G(RA(w))) + uint32_t(I7(w)), 8); return true; }
bool op_cbx(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = insert_mask(pref(G(RA(w))) + pref(G(RB(w))), 1); return true; }
bool op_chx(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = insert_mask(pref(G(RA(w))) + pref(G(RB(w))), 2); return true; }
bool op_cwx(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = insert_mask(pref(G(RA(w))) + pref(G(RB(w))), 4); return true; }
bool op_cdx(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = insert_mask(pref(G(RA(w))) + pref(G(RB(w))), 8); return true; }

bool op_ceq(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpeq_epi32(G(RA(w)).vi, G(RB(w)).vi); return true; }
bool op_ceqh(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpeq_epi16(G(RA(w)).vi, G(RB(w)).vi); return true; }
bool op_ceqb(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpeq_epi8(G(RA(w)).vi, G(RB(w)).vi); return true; }
bool op_ceqi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpeq_epi32(G(RA(w)).vi, _mm_set1_epi32(I10(w))); return true; }
bool op_ceqhi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpeq_epi16(G(RA(w)).vi, _mm_set1_epi16(static_cast<int16_t>(I10(w)))); return true; }
bool op_ceqbi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpeq_epi8(G(RA(w)).vi, _mm_set1_epi8(static_cast<int8_t>(I10(w)))); return true; }
bool op_cgt(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpgt_epi32(G(RA(w)).vi, G(RB(w)).vi); return true; }
bool op_cgth(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpgt_epi16(G(RA(w)).vi, G(RB(w)).vi); return true; }
bool op_cgtb(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpgt_epi8(G(RA(w)).vi, G(RB(w)).vi); return true; }
bool op_cgti(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpgt_epi32(G(RA(w)).vi, _mm_set1_epi32(I10(w))); return true; }
bool op_cgthi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpgt_epi16(G(RA(w)).vi, _mm_set1_epi16(static_cast<int16_t>(I10(w)))); return true; }
bool op_cgtbi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)).vi = _mm_cmpgt_epi8(G(RA(w)).vi, _mm_set1_epi8(static_cast<int8_t>(I10(w)))); return true; }
bool op_clgt(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map32(G(RA(w)), G(RB(w)), [](uint32_t x, uint32_t y) { return x > y ? 0xFFFFFFFFu : 0u; }); return true; }
bool op_clgth(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map16(G(RA(w)), G(RB(w)), [](uint16_t x, uint16_t y) { return uint16_t(x > y ? 0xFFFF : 0); }); return true; }
bool op_clgtb(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = map8(G(RA(w)), G(RB(w)), [](uint8_t x, uint8_t y) { return uint8_t(x > y ? 0xFF : 0); }); return true; }
bool op_clgti(Context& c, Host&, uint32_t w, uint32_t) { uint32_t i = uint32_t(I10(w)); G(RT(w)) = map32(G(RA(w)), G(RA(w)), [i](uint32_t x, uint32_t) { return x > i ? 0xFFFFFFFFu : 0u; }); return true; }
bool op_clgthi(Context& c, Host&, uint32_t w, uint32_t) { uint16_t i = uint16_t(I10(w)); G(RT(w)) = map16(G(RA(w)), G(RA(w)), [i](uint16_t x, uint16_t) { return uint16_t(x > i ? 0xFFFF : 0); }); return true; }
bool op_clgtbi(Context& c, Host&, uint32_t w, uint32_t) { uint8_t i = uint8_t(I10(w)); G(RT(w)) = map8(G(RA(w)), G(RA(w)), [i](uint8_t x, uint8_t) { return uint8_t(x > i ? 0xFF : 0); }); return true; }

bool halt_if(Context& c, Host& h, bool cond, uint32_t w, uint32_t pc) {
    if (cond) {
        h.halt(pc, w);
        c.halted = true;
        return false;
    }
    return true;
}
bool op_heq(Context& c, Host& h, uint32_t w, uint32_t pc) { return halt_if(c, h, pref(G(RA(w))) == pref(G(RB(w))), w, pc); }
bool op_heqi(Context& c, Host& h, uint32_t w, uint32_t pc) { return halt_if(c, h, int32_t(pref(G(RA(w)))) == I10(w), w, pc); }
bool op_hgt(Context& c, Host& h, uint32_t w, uint32_t pc) { return halt_if(c, h, int32_t(pref(G(RA(w)))) > int32_t(pref(G(RB(w)))), w, pc); }
bool op_hgti(Context& c, Host& h, uint32_t w, uint32_t pc) { return halt_if(c, h, int32_t(pref(G(RA(w)))) > I10(w), w, pc); }
bool op_hlgt(Context& c, Host& h, uint32_t w, uint32_t pc) { return halt_if(c, h, pref(G(RA(w))) > pref(G(RB(w))), w, pc); }
bool op_hlgti(Context& c, Host& h, uint32_t w, uint32_t pc) { return halt_if(c, h, pref(G(RA(w))) > uint32_t(I10(w)), w, pc); }

template <class F>
inline v128 mapf(const v128& a, const v128& b, F f) { v128 r; for (int i = 0; i < 4; i++) r.f32[i] = f(a.f32[i], b.f32[i]); return r; }

bool op_fa(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = mapf(G(RA(w)), G(RB(w)), [](float x, float y) { return fclamp(x + y); }); return true; }
bool op_fs(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = mapf(G(RA(w)), G(RB(w)), [](float x, float y) { return fclamp(x - fclamp(y)); }); return true; }
bool op_fm(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = mapf(G(RA(w)), G(RB(w)), [](float x, float y) { return fzero_mask(fclamp(x * y), x, y); }); return true; }
bool op_fma(Context& c, Host&, uint32_t w, uint32_t) {
    v128 a = G(RA(w)), b = G(RB(w)), cc = G(RRR_C(w)), r;
    for (int i = 0; i < 4; i++) r.f32[i] = (a.f32[i] == 0.0f || b.f32[i] == 0.0f) ? cc.f32[i] : fclamp(std::fma(a.f32[i], b.f32[i], cc.f32[i]));
    G(RRR_T(w)) = r;
    return true;
}
bool op_fms(Context& c, Host&, uint32_t w, uint32_t) {
    v128 a = G(RA(w)), b = G(RB(w)), cc = G(RRR_C(w)), r;
    for (int i = 0; i < 4; i++) r.f32[i] = (a.f32[i] == 0.0f || b.f32[i] == 0.0f) ? -cc.f32[i] : fclamp(std::fma(a.f32[i], b.f32[i], -cc.f32[i]));
    G(RRR_T(w)) = r;
    return true;
}
bool op_fnms(Context& c, Host&, uint32_t w, uint32_t) {
    v128 a = G(RA(w)), b = G(RB(w)), cc = G(RRR_C(w)), r;
    for (int i = 0; i < 4; i++) r.f32[i] = (a.f32[i] == 0.0f || b.f32[i] == 0.0f) ? cc.f32[i] : fclamp(std::fma(-a.f32[i], b.f32[i], cc.f32[i]));
    G(RRR_T(w)) = r;
    return true;
}
bool op_frest(Context& c, Host&, uint32_t w, uint32_t) {
    v128 a = G(RA(w)), r;
    for (int i = 0; i < 4; i++) {
        uint32_t bits = a.u32[i];
        if ((bits & 0x7F800000) == 0) r.u32[i] = (bits & 0x80000000) | 0x7FFFFFFF;
        else r.f32[i] = fclamp(1.0f / a.f32[i]);
    }
    G(RT(w)) = r;
    return true;
}
bool op_frsqest(Context& c, Host&, uint32_t w, uint32_t) {
    v128 a = G(RA(w)), r;
    for (int i = 0; i < 4; i++) {
        uint32_t bits = a.u32[i] & 0x7FFFFFFF;
        if ((bits & 0x7F800000) == 0) r.u32[i] = 0x7FFFFFFF;
        else r.f32[i] = fclamp(1.0f / std::sqrt(std::bit_cast<float>(bits)));
    }
    G(RT(w)) = r;
    return true;
}
bool op_fi(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = G(RB(w)); return true; }
bool op_fcgt(Context& c, Host&, uint32_t w, uint32_t) { v128 a = G(RA(w)), b = G(RB(w)), r; for (int i = 0; i < 4; i++) r.u32[i] = a.f32[i] > b.f32[i] ? 0xFFFFFFFF : 0; G(RT(w)) = r; return true; }
bool op_fcmgt(Context& c, Host&, uint32_t w, uint32_t) { v128 a = G(RA(w)), b = G(RB(w)), r; for (int i = 0; i < 4; i++) r.u32[i] = std::fabs(a.f32[i]) > std::fabs(b.f32[i]) ? 0xFFFFFFFF : 0; G(RT(w)) = r; return true; }
bool op_fceq(Context& c, Host&, uint32_t w, uint32_t) { v128 a = G(RA(w)), b = G(RB(w)), r; for (int i = 0; i < 4; i++) r.u32[i] = a.f32[i] == b.f32[i] ? 0xFFFFFFFF : 0; G(RT(w)) = r; return true; }
bool op_fcmeq(Context& c, Host&, uint32_t w, uint32_t) { v128 a = G(RA(w)), b = G(RB(w)), r; for (int i = 0; i < 4; i++) r.u32[i] = std::fabs(a.f32[i]) == std::fabs(b.f32[i]) ? 0xFFFFFFFF : 0; G(RT(w)) = r; return true; }

bool op_cflts(Context& c, Host&, uint32_t w, uint32_t) {
    double scale = std::ldexp(1.0, int(173 - I8(w)));
    v128 a = G(RA(w)), r;
    for (int i = 0; i < 4; i++) {
        double v = double(a.f32[i]) * scale;
        r.s32[i] = v >= 2147483647.0 ? INT32_MAX : v <= -2147483648.0 ? INT32_MIN : int32_t(v);
    }
    G(RT(w)) = r;
    return true;
}
bool op_cfltu(Context& c, Host&, uint32_t w, uint32_t) {
    double scale = std::ldexp(1.0, int(173 - I8(w)));
    v128 a = G(RA(w)), r;
    for (int i = 0; i < 4; i++) {
        double v = double(a.f32[i]) * scale;
        r.u32[i] = v >= 4294967295.0 ? UINT32_MAX : v <= 0.0 ? 0 : uint32_t(v);
    }
    G(RT(w)) = r;
    return true;
}
bool op_csflt(Context& c, Host&, uint32_t w, uint32_t) {
    double scale = std::ldexp(1.0, int(I8(w)) - 155);
    v128 a = G(RA(w)), r;
    for (int i = 0; i < 4; i++) r.f32[i] = float(double(a.s32[i]) * scale);
    G(RT(w)) = r;
    return true;
}
bool op_cuflt(Context& c, Host&, uint32_t w, uint32_t) {
    double scale = std::ldexp(1.0, int(I8(w)) - 155);
    v128 a = G(RA(w)), r;
    for (int i = 0; i < 4; i++) r.f32[i] = float(double(a.u32[i]) * scale);
    G(RT(w)) = r;
    return true;
}
bool op_fesd(Context& c, Host&, uint32_t w, uint32_t) { RoundGuard g; v128 a = G(RA(w)), r; r.u64[1] = std::bit_cast<uint64_t>(double(a.f32[3])); r.u64[0] = std::bit_cast<uint64_t>(double(a.f32[1])); G(RT(w)) = r; return true; }
bool op_frds(Context& c, Host&, uint32_t w, uint32_t) { RoundGuard g; v128 a = G(RA(w)), r{}; r.f32[3] = float(std::bit_cast<double>(a.u64[1])); r.f32[1] = float(std::bit_cast<double>(a.u64[0])); G(RT(w)) = r; return true; }

template <class F>
inline v128 mapd(const v128& a, const v128& b, F f) { RoundGuard g; v128 r; for (int i = 0; i < 2; i++) r.u64[i] = std::bit_cast<uint64_t>(f(std::bit_cast<double>(a.u64[i]), std::bit_cast<double>(b.u64[i]))); return r; }
bool op_dfa(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = mapd(G(RA(w)), G(RB(w)), [](double x, double y) { return x + y; }); return true; }
bool op_dfs(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = mapd(G(RA(w)), G(RB(w)), [](double x, double y) { return x - y; }); return true; }
bool op_dfm(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = mapd(G(RA(w)), G(RB(w)), [](double x, double y) { return x * y; }); return true; }
bool dfm_family(Context& c, uint32_t w, int kind) {
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
bool op_dfma(Context& c, Host&, uint32_t w, uint32_t) { return dfm_family(c, w, 0); }
bool op_dfms(Context& c, Host&, uint32_t w, uint32_t) { return dfm_family(c, w, 1); }
bool op_dfnms(Context& c, Host&, uint32_t w, uint32_t) { return dfm_family(c, w, 2); }
bool op_dfnma(Context& c, Host&, uint32_t w, uint32_t) { return dfm_family(c, w, 3); }
bool dcmp(Context& c, uint32_t w, int kind) {
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
bool op_dfcgt(Context& c, Host&, uint32_t w, uint32_t) { return dcmp(c, w, 0); }
bool op_dfceq(Context& c, Host&, uint32_t w, uint32_t) { return dcmp(c, w, 1); }
bool op_dfcmgt(Context& c, Host&, uint32_t w, uint32_t) { return dcmp(c, w, 2); }
bool op_dfcmeq(Context& c, Host&, uint32_t w, uint32_t) { return dcmp(c, w, 3); }
bool op_dftsv(Context& c, Host&, uint32_t w, uint32_t) {
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
bool op_fscrrd(Context& c, Host&, uint32_t w, uint32_t) { G(RT(w)) = v128{}; return true; }
bool op_fscrwr(Context&, Host&, uint32_t, uint32_t) { return true; }

#undef G

struct Table {
    Handler h[2048];

    void set(uint32_t opcode, int bits, Handler fn) {
        int shift = 11 - bits;
        for (uint32_t k = 0; k < (1u << shift); k++) h[(opcode << shift) | k] = fn;
    }

    Table() {
        for (auto& e : h) e = op_illegal;
        set(0x8, 4, op_selb); set(0xB, 4, op_shufb); set(0xC, 4, op_mpya); set(0xD, 4, op_fnms); set(0xE, 4, op_fma); set(0xF, 4, op_fms);
        set(0x21, 7, op_ila); set(0x08, 7, op_nop); set(0x09, 7, op_nop);
        set(0x04, 8, op_ori); set(0x05, 8, op_orhi); set(0x06, 8, op_orbi); set(0x0C, 8, op_sfi); set(0x0D, 8, op_sfhi);
        set(0x14, 8, op_andi); set(0x15, 8, op_andhi); set(0x16, 8, op_andbi); set(0x1C, 8, op_ai); set(0x1D, 8, op_ahi);
        set(0x24, 8, op_stqd); set(0x34, 8, op_lqd); set(0x44, 8, op_xori); set(0x45, 8, op_xorhi); set(0x46, 8, op_xorbi);
        set(0x4C, 8, op_cgti); set(0x4D, 8, op_cgthi); set(0x4E, 8, op_cgtbi); set(0x4F, 8, op_hgti);
        set(0x5C, 8, op_clgti); set(0x5D, 8, op_clgthi); set(0x5E, 8, op_clgtbi); set(0x5F, 8, op_hlgti);
        set(0x74, 8, op_mpyi); set(0x75, 8, op_mpyui); set(0x7C, 8, op_ceqi); set(0x7D, 8, op_ceqhi); set(0x7E, 8, op_ceqbi); set(0x7F, 8, op_heqi);
        set(0x040, 9, op_brz); set(0x041, 9, op_stqa); set(0x042, 9, op_brnz); set(0x044, 9, op_brhz); set(0x046, 9, op_brhnz); set(0x047, 9, op_stqr);
        set(0x060, 9, op_bra); set(0x061, 9, op_lqa); set(0x062, 9, op_brasl); set(0x064, 9, op_br); set(0x065, 9, op_fsmbi); set(0x066, 9, op_brsl); set(0x067, 9, op_lqr);
        set(0x081, 9, op_il); set(0x082, 9, op_ilhu); set(0x083, 9, op_ilh); set(0x0C1, 9, op_iohl);
        set(0x1D8, 10, op_cflts); set(0x1D9, 10, op_cfltu); set(0x1DA, 10, op_csflt); set(0x1DB, 10, op_cuflt);
        set(0x000, 11, op_stop); set(0x001, 11, op_nop); set(0x002, 11, op_nop); set(0x003, 11, op_nop); set(0x00C, 11, op_mfspr); set(0x00D, 11, op_rdch); set(0x00F, 11, op_rchcnt);
        set(0x040, 11, op_sf); set(0x041, 11, op_or); set(0x042, 11, op_bg); set(0x048, 11, op_sfh); set(0x049, 11, op_nor); set(0x053, 11, op_absdb);
        set(0x058, 11, op_rot); set(0x059, 11, op_rotm); set(0x05A, 11, op_rotma); set(0x05B, 11, op_shl); set(0x05C, 11, op_roth); set(0x05D, 11, op_rothm); set(0x05E, 11, op_rotmah); set(0x05F, 11, op_shlh);
        set(0x078, 11, op_roti); set(0x079, 11, op_rotmi); set(0x07A, 11, op_rotmai); set(0x07B, 11, op_shli); set(0x07C, 11, op_rothi); set(0x07D, 11, op_rothmi); set(0x07E, 11, op_rotmahi); set(0x07F, 11, op_shlhi);
        set(0x0C0, 11, op_a); set(0x0C1, 11, op_and); set(0x0C2, 11, op_cg); set(0x0C8, 11, op_ah); set(0x0C9, 11, op_nand); set(0x0D3, 11, op_avgb);
        set(0x10C, 11, op_nop); set(0x10D, 11, op_wrch);
        set(0x128, 11, op_biz); set(0x129, 11, op_binz); set(0x12A, 11, op_bihz); set(0x12B, 11, op_bihnz);
        set(0x140, 11, op_stop); set(0x144, 11, op_stqx);
        set(0x1A8, 11, op_bi); set(0x1A9, 11, op_bisl); set(0x1AA, 11, op_iret); set(0x1AB, 11, op_bisled); set(0x1AC, 11, op_nop);
        set(0x1B0, 11, op_gb); set(0x1B1, 11, op_gbh); set(0x1B2, 11, op_gbb); set(0x1B4, 11, op_fsm); set(0x1B5, 11, op_fsmh); set(0x1B6, 11, op_fsmb);
        set(0x1B8, 11, op_frest); set(0x1B9, 11, op_frsqest); set(0x1C4, 11, op_lqx);
        set(0x1CC, 11, op_rotqbybi); set(0x1CD, 11, op_rotqmbybi); set(0x1CF, 11, op_shlqbybi);
        set(0x1D4, 11, op_cbx); set(0x1D5, 11, op_chx); set(0x1D6, 11, op_cwx); set(0x1D7, 11, op_cdx);
        set(0x1D8, 11, op_rotqbi); set(0x1D9, 11, op_rotqmbi); set(0x1DB, 11, op_shlqbi); set(0x1DC, 11, op_rotqby); set(0x1DD, 11, op_rotqmby); set(0x1DF, 11, op_shlqby);
        set(0x1F0, 11, op_orx); set(0x1F4, 11, op_cbd); set(0x1F5, 11, op_chd); set(0x1F6, 11, op_cwd); set(0x1F7, 11, op_cdd);
        set(0x1F8, 11, op_rotqbii); set(0x1F9, 11, op_rotqmbii); set(0x1FB, 11, op_shlqbii); set(0x1FC, 11, op_rotqbyi); set(0x1FD, 11, op_rotqmbyi); set(0x1FF, 11, op_shlqbyi);
        set(0x201, 11, op_nop);
        set(0x240, 11, op_cgt); set(0x241, 11, op_xor); set(0x248, 11, op_cgth); set(0x249, 11, op_eqv); set(0x250, 11, op_cgtb); set(0x253, 11, op_sumb); set(0x258, 11, op_hgt);
        set(0x2A5, 11, op_clz); set(0x2A6, 11, op_xswd); set(0x2AE, 11, op_xshw); set(0x2B4, 11, op_cntb); set(0x2B6, 11, op_xsbh);
        set(0x2C0, 11, op_clgt); set(0x2C1, 11, op_andc); set(0x2C2, 11, op_fcgt); set(0x2C3, 11, op_dfcgt); set(0x2C4, 11, op_fa); set(0x2C5, 11, op_fs); set(0x2C6, 11, op_fm);
        set(0x2C8, 11, op_clgth); set(0x2C9, 11, op_orc); set(0x2CA, 11, op_fcmgt); set(0x2CB, 11, op_dfcmgt); set(0x2CC, 11, op_dfa); set(0x2CD, 11, op_dfs); set(0x2CE, 11, op_dfm);
        set(0x2D0, 11, op_clgtb); set(0x2D8, 11, op_hlgt);
        set(0x340, 11, op_addx); set(0x341, 11, op_sfx); set(0x342, 11, op_cgx); set(0x343, 11, op_bgx); set(0x346, 11, op_mpyhha); set(0x34E, 11, op_mpyhhau);
        set(0x35C, 11, op_dfma); set(0x35D, 11, op_dfms); set(0x35E, 11, op_dfnms); set(0x35F, 11, op_dfnma);
        set(0x398, 11, op_fscrrd); set(0x3B8, 11, op_fesd); set(0x3B9, 11, op_frds); set(0x3BA, 11, op_fscrwr); set(0x3BF, 11, op_dftsv);
        set(0x3C0, 11, op_ceq); set(0x3C2, 11, op_fceq); set(0x3C3, 11, op_dfceq); set(0x3C4, 11, op_mpy); set(0x3C5, 11, op_mpyh); set(0x3C6, 11, op_mpyhh); set(0x3C7, 11, op_mpys);
        set(0x3C8, 11, op_ceqh); set(0x3CA, 11, op_fcmeq); set(0x3CB, 11, op_dfcmeq); set(0x3CC, 11, op_mpyu); set(0x3CE, 11, op_mpyhhu); set(0x3D0, 11, op_ceqb); set(0x3D4, 11, op_fi); set(0x3D8, 11, op_heq);
    }
};

const Table g_table;

}

bool step(Context& c, Host& host) {
    uint32_t pc = c.pc;
    uint32_t w;
    std::memcpy(&w, c.ls + pc, 4);
    w = __builtin_bswap32(w);
    c.pc = (pc + 4) & PC_MASK;
    return g_table.h[w >> 21](c, host, w, pc);
}

void interpret(Context& c, Host& host) {
    while (step(c, host)) {
    }
}

}
