#include <immintrin.h>
#include "ppu/ops.h"
#include "runtime/reservation.h"

#include <atomic>
#include <cmath>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "runtime/host.h"

namespace ppu {

void trap(Context& ctx, uint8_t* base, uint32_t pc) {
    std::fprintf(stderr, "[ppu] trap at 0x%08x\n", pc);
    std::abort();
}

void unimplemented(Context& ctx, uint8_t* base, uint32_t pc, uint32_t word) {
    std::fprintf(stderr, "[ppu] unimplemented instruction 0x%08x at 0x%08x\n", word, pc);
    std::abort();
}

namespace {

uint64_t host_ticks() {
    static const double scale = [] {
        return 80000000.0 / static_cast<double>(host::qpf());
    }();
    return static_cast<uint64_t>(static_cast<double>(host::qpc()) * scale);
}

struct Turbo {
    double factor = 1.0;
    uint64_t start = 0;
    uint64_t until = 0;
};

const Turbo& turbo() {
    static const Turbo t = [] {
        Turbo t;
        t.start = host_ticks();
        if (const char* e = std::getenv("GOWA_TURBO")) {
            t.factor = std::atof(e);
            const char* at = std::strchr(e, '@');
            t.until = static_cast<uint64_t>((at ? std::atof(at + 1) : 1e6) * 80000000.0);
            if (t.factor < 1.0) t.factor = 1.0;
        }
        return t;
    }();
    return t;
}

}

uint64_t timebase() {
    const Turbo& t = turbo();
    uint64_t raw = host_ticks();
    if (t.factor == 1.0) return raw;
    uint64_t rel = raw - t.start;
    if (rel < t.until) return t.start + static_cast<uint64_t>(rel * t.factor);
    return t.start + static_cast<uint64_t>(t.until * t.factor) + (rel - t.until);
}

double time_scale() {
    const Turbo& t = turbo();
    if (t.factor == 1.0) return 1.0;
    return host_ticks() - t.start < t.until ? t.factor : 1.0;
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

uint8_t hook_flags[0x800000];
void (*hook_impl)(Context& ctx, uint8_t* base, uint32_t addr, BodyFn fn) = nullptr;

void hook_call(Context& ctx, uint8_t* base, uint32_t addr, BodyFn fn) {
    if (hook_impl) hook_impl(ctx, base, addr, fn);
    else fn(ctx, base);
}

double fres(double x) {
    if (std::isnan(x)) return u2f(f2u(x) | 0x0008000000000000ull);
    return static_cast<double>(static_cast<float>(1.0 / x));
}

double frsqrte(double x) {
    if (std::isnan(x)) return u2f(f2u(x) | 0x0008000000000000ull);
    if (x == 0.0) return std::signbit(x) ? -HUGE_VAL : HUGE_VAL;
    if (x < 0.0) return u2f(0x7FF8000000000000ull);
    return 1.0 / std::sqrt(x);
}
float vrefp(float x) { return 1.0f / x; }

uint32_t vlogefp(uint32_t x, bool nj) {
    uint32_t e = (x >> 23) & 0xFF, m = x & 0x7FFFFF;
    if (e == 0xFF) return m ? x | 0x400000 : (x >> 31) ? 0x7FC00000u : x;
    if (e == 0 && (nj || m == 0)) return 0xFF800000u;
    if (x >> 31) return 0x7FC00000u;
    float r = std::log2(std::bit_cast<float>(x));
    return std::bit_cast<uint32_t>(r);
}

uint32_t vexptefp(uint32_t x, bool nj) {
    uint32_t e = (x >> 23) & 0xFF, m = x & 0x7FFFFF;
    if (e == 0xFF) return m ? x | 0x400000 : (x >> 31) ? 0u : x;
    float v = std::bit_cast<float>(x);
    if (nj && e == 0) v = 0.0f;
    float r = std::exp2(v);
    uint32_t b = std::bit_cast<uint32_t>(r);
    if (nj && ((b >> 23) & 0xFF) == 0) b &= 0x80000000u;
    return b;
}
float vrsqrtefp(float x) { return 1.0f / std::sqrt(x); }

}
