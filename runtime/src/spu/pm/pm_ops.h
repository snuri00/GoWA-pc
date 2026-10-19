#pragma once

#include "../renjob/renjob_ops.h"

#include "spu/context.h"

namespace pm::gen {

using namespace renjob::xo;
using renjob::xo::bg;
using renjob::xo::bgx;
using renjob::xo::cbd;
using renjob::xo::cbx;
using renjob::xo::cdd;
using renjob::xo::cdx;
using renjob::xo::cgx;
using renjob::xo::chd;
using renjob::xo::chx;
using renjob::xo::clz;
using renjob::xo::cwd;
using renjob::xo::cwx;
using renjob::xo::gbb;
using renjob::xo::mpya;
using renjob::xo::mpys;
using renjob::xo::orx;
using renjob::xo::rothi;
using renjob::xo::rotqbi;
using renjob::xo::rotqbii;
using renjob::xo::rotqby;
using renjob::xo::rotqbybi;
using renjob::xo::rotqmbi;
using renjob::xo::rotqmbii;
using renjob::xo::rotqmby;
using renjob::xo::rotqmbybi;
using renjob::xo::sfx;
using renjob::xo::shlh;
using renjob::xo::shlqbi;
using renjob::xo::shlqby;
using renjob::xo::shlqbybi;

inline __attribute__((always_inline)) void stm(spu::Context& c, uint8_t* ls, uint32_t a, V v) {
    a &= 0x3fff0;
    _mm_storeu_si128(reinterpret_cast<__m128i*>(ls + a), bswap(v));
    if (c.line_epoch) c.line_epoch[a >> 7].store(c.epoch->load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

inline uint32_t run_trampoline(spu::Context& c, uint8_t* ls, V* R, uint64_t& cnt, uint32_t link) {
    uint32_t pc = 0x3fec0;
    uint32_t n = 0;
    for (; n < 128 && pc < 0x40000; n++, pc += 4) {
        uint32_t w;
        std::memcpy(&w, ls + pc, 4);
        w = __builtin_bswap32(w);
        uint32_t rt = w & 0x7f;
        uint32_t op9 = w >> 23;
        uint32_t addr = (uint32_t(int32_t(w << 9) >> 16) << 2) & 0x3fff0;
        if (op9 == 0x061) {
            R[rt] = lq(ls, addr);
        } else if (op9 == 0x041) {
            stm(c, ls, addr, R[rt]);
        } else if ((w >> 21) == 0x1a8 && ((w >> 7) & 0x7f) == link && ((w >> 14) & 0x7f) == 0) {
            cnt += n + 1;
            return 0;
        } else {
            break;
        }
    }
    cnt += n;
    return pc;
}

}
