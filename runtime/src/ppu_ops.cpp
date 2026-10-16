#include "ppu/ops.h"
#include "ppu/estimate_tables.h"

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
        return 79800000.0 / static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(static_cast<double>(c.QuadPart) * scale);
}

uint32_t lwarx(Context& ctx, uint8_t* base, uint32_t ea) {
    ctx.raddr = ea;
    uint32_t v = ld32(base, ea);
    ctx.rtime = v;
    return v;
}

uint64_t ldarx(Context& ctx, uint8_t* base, uint32_t ea) {
    ctx.raddr = ea | 1;
    uint64_t v = ld64(base, ea);
    ctx.rtime = v;
    return v;
}

bool stwcx(Context& ctx, uint8_t* base, uint32_t ea, uint32_t value) {
    if (ctx.raddr != ea) return false;
    ctx.raddr = 0;
    auto* p = reinterpret_cast<std::atomic<uint32_t>*>(base + ea);
    uint32_t expected = __builtin_bswap32(static_cast<uint32_t>(ctx.rtime));
    return p->compare_exchange_strong(expected, __builtin_bswap32(value));
}

bool stdcx(Context& ctx, uint8_t* base, uint32_t ea, uint64_t value) {
    if (ctx.raddr != (ea | 1)) return false;
    ctx.raddr = 0;
    auto* p = reinterpret_cast<std::atomic<uint64_t>*>(base + ea);
    uint64_t expected = __builtin_bswap64(ctx.rtime);
    return p->compare_exchange_strong(expected, __builtin_bswap64(value));
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
