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

}
