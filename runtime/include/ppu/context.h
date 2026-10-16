#pragma once

#include <cstdint>
#include <cstring>
#include <immintrin.h>

namespace ppu {

union alignas(16) v128 {
    uint8_t u8[16];
    int8_t s8[16];
    uint16_t u16[8];
    int16_t s16[8];
    uint32_t u32[4];
    int32_t s32[4];
    uint64_t u64[2];
    float f32[4];
    __m128i vi;
    __m128 vf;
};

struct CRField {
    uint8_t lt;
    uint8_t gt;
    uint8_t eq;
    uint8_t so;
};

struct Context {
    uint64_t gpr[32];
    double fpr[32];
    v128 vr[32];
    CRField cr[8];
    uint64_t lr;
    uint64_t ctr;
    uint8_t xer_so;
    uint8_t xer_ov;
    uint8_t xer_ca;
    uint8_t xer_cnt;
    uint32_t fpscr;
    uint32_t vscr_sat;
    uint32_t vscr_nj;
    uint32_t raddr;
    uint64_t rtime;
    alignas(64) uint8_t rdata[128];
    uint32_t cia;
    uint32_t thread_id;
    void* host;
};

using Func = void (*)(Context&, uint8_t*);

}
