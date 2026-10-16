#include <immintrin.h>
#include "ppu/ops.h"
#include "ppu/estimate_tables.h"
#include "runtime/reservation.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace ppu {

void trap(Context& ctx, uint8_t* base, uint32_t pc) {
    std::fprintf(stderr, "[ppu] trap at 0x%08x\n", pc);
    std::abort();
}

void unimplemented(Context& ctx, uint8_t* base, uint32_t pc, uint32_t word) {
    std::fprintf(stderr, "[ppu] unimplemented instruction 0x%08x at 0x%08x\n", word, pc);
    std::abort();
}

uint64_t timebase() {
    static const double scale = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 80000000.0 / static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(static_cast<double>(c.QuadPart) * scale);
}

namespace {

void reserve(Context& ctx, uint8_t* base, uint32_t blk) {
    auto& ver = rsv::version(blk);
    for (;;) {
        uint64_t v1 = ver.load(std::memory_order_acquire);
        if (v1 & 64) {
            _mm_pause();
            continue;
        }
        std::memcpy(ctx.rdata, base + blk, 8);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (ver.load(std::memory_order_acquire) == v1) {
            ctx.rtime = v1;
            return;
        }
    }
}

}

uint32_t lwarx(Context& ctx, uint8_t* base, uint32_t ea) {
    uint32_t blk = ea & ~7u;
    reserve(ctx, base, blk);
    ctx.raddr = ea | 1;
    uint32_t v;
    std::memcpy(&v, ctx.rdata + (ea & 7u), 4);
    return __builtin_bswap32(v);
}

uint64_t ldarx(Context& ctx, uint8_t* base, uint32_t ea) {
    uint32_t blk = ea & ~7u;
    reserve(ctx, base, blk);
    ctx.raddr = ea | 1;
    uint64_t v;
    std::memcpy(&v, ctx.rdata, 8);
    return __builtin_bswap64(v);
}

namespace {

bool store_conditional(Context& ctx, uint8_t* base, uint32_t ea, const void* bytes, uint32_t size) {
    if (!(ctx.raddr & 1) || ((ctx.raddr & ~7u) != (ea & ~7u))) {
        ctx.raddr = 0;
        return false;
    }
    ctx.raddr = 0;
    uint32_t blk = ea & ~7u;
    uint8_t data[8];
    std::memcpy(data, ctx.rdata, 8);
    std::memcpy(data + (ea - blk), bytes, size);
    return rsv::commit_if(blk & ~127u, ctx.rtime, ctx.rdata, data, blk & 127u, 8);
}

}

bool stwcx(Context& ctx, uint8_t* base, uint32_t ea, uint32_t value) {
    uint32_t be = __builtin_bswap32(value);
    return store_conditional(ctx, base, ea, &be, 4);
}

bool stdcx(Context& ctx, uint8_t* base, uint32_t ea, uint64_t value) {
    uint64_t be = __builtin_bswap64(value);
    return store_conditional(ctx, base, ea, &be, 8);
}

void notify_store(uint8_t* base, uint32_t ea, uint32_t size) {}

double fres(double x) {
    uint64_t b = f2u(x);
    if (std::isnan(x)) return u2f(b | 0x0008000000000000ull);
    uint64_t e = (b >> 52) & 0x7FF;
    uint64_t m = tables::fres_mantissa[(b >> 45) & 0x7F];
    uint64_t r = e >= 0x47F ? 0 : ((0x7FDull - e) << 52) | (m << 29);
    return u2f((b & 0x8000000000000000ull) | r);
}

double frsqrte(double x) {
    uint64_t b = f2u(x);
    uint32_t key = static_cast<uint32_t>(b >> 49);
    uint32_t sign = key >> 14;
    uint32_t e = (key >> 3) & 0x7FF;
    uint32_t hi;
    if (e == 0) hi = 0x7FF00000u | (sign << 31);
    else if (e == 0x7FF) hi = (key == (0x7FFu << 3)) ? 0 : 0x7FF80000u;
    else if (sign) hi = 0x7FF80000u;
    else hi = tables::frsqrte_mantissa[8 ^ (key & 15)] | (0x3FE00000u - (((e + 0x1C01u) >> 1) << 20));
    return u2f(uint64_t(hi) << 32);
}
float vrefp(float x) { return 1.0f / x; }
uint32_t vlogefp(uint32_t x, bool nj) { return tables::ppu_vlogefp(x, nj); }
uint32_t vexptefp(uint32_t x, bool nj) { return tables::ppu_vexptefp(x, nj); }
float vrsqrtefp(float x) { return 1.0f / std::sqrt(x); }

}
