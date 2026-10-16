#include "ppu/ops.h"
#include "runtime/image.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace ppu {

namespace {

std::vector<Func> g_dispatch;

void build_dispatch() {
    g_dispatch.assign((image::text_hi - image::text_lo) / 4, nullptr);
    for (uint32_t i = 0; i < image::function_count; i++) {
        g_dispatch[(image::functions[i].addr - image::text_lo) / 4] = image::functions[i].fn;
    }
}

Func lookup(uint32_t addr) {
    if (g_dispatch.empty()) build_dispatch();
    if (addr < image::text_lo || addr >= image::text_hi || (addr & 3)) return nullptr;
    return g_dispatch[(addr - image::text_lo) / 4];
}

}

void call_indirect(Context& ctx, uint8_t* base, uint32_t addr) {
    Func fn = lookup(addr);
    if (!fn) {
        std::fprintf(stderr, "[ppu] unknown indirect target 0x%08x (lr=0x%08llx)\n", addr, static_cast<unsigned long long>(ctx.lr));
        std::abort();
    }
    fn(ctx, base);
}

void call_import(Context& ctx, uint8_t* base, uint32_t index) {
    const image::ImportEntry& e = image::imports[index];
    std::fprintf(stderr, "[import] %s::%s (0x%08x) unimplemented\n", e.module, e.name ? e.name : "?", e.nid);
    ctx.gpr[3] = 0;
}

void syscall(Context& ctx, uint8_t* base) {
    std::fprintf(stderr, "[lv2] syscall %llu at 0x%08x unimplemented\n", static_cast<unsigned long long>(ctx.gpr[11]), ctx.cia);
    ctx.gpr[3] = 0;
}

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

double fres(double x) { return 1.0 / x; }
double frsqrte(double x) { return 1.0 / std::sqrt(x); }
float vrefp(float x) { return 1.0f / x; }
float vrsqrtefp(float x) { return 1.0f / std::sqrt(x); }

}
