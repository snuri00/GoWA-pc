#include "common.h"

namespace hle {

namespace {

bool g_init = false;
uint32_t g_max = 7;
bool g_first = true;

}

HLE_FUNC(sys_io, cellPadInit) {
    if (g_init) return ret(ctx, static_cast<int32_t>(0x80121103));
    g_init = true;
    g_max = a32(ctx, 0) > 7 ? 7 : a32(ctx, 0);
    ret(ctx, 0);
}

HLE_FUNC(sys_io, cellPadEnd) {
    g_init = false;
    ret(ctx, 0);
}

HLE_FUNC(sys_io, cellPadGetInfo2) {
    uint32_t out = a32(ctx, 0);
    if (!g_init) return ret(ctx, static_cast<int32_t>(0x80121104));
    std::memset(vm::ptr(out), 0, 4 * 3 + 4 * 7 * 4);
    vm::write32(out, g_max);
    vm::write32(out + 4, 1);
    vm::write32(out + 8, 0);
    vm::write32(out + 12, 1);
    vm::write32(out + 12 + 28, 0);
    vm::write32(out + 12 + 56, 0x7);
    vm::write32(out + 12 + 84, 0);
    ret(ctx, 0);
}

HLE_FUNC(sys_io, cellPadGetData) {
    uint32_t port = a32(ctx, 0), out = a32(ctx, 1);
    if (!g_init) return ret(ctx, static_cast<int32_t>(0x80121104));
    std::memset(vm::ptr(out), 0, 4 + 64 * 2);
    if (port != 0) return ret(ctx, static_cast<int32_t>(0x80121107));
    if (g_first) {
        vm::write32(out, 24);
        vm::write16(out + 4, 0);
        vm::write16(out + 6, 0x7300);
        for (int i = 4; i < 8; i++) vm::write16(out + 4 + i * 2, 0x80);
        for (int i = 20; i < 24; i++) vm::write16(out + 4 + i * 2, 0x200);
        g_first = false;
    }
    ret(ctx, 0);
}

HLE_FUNC(sys_io, cellPadClearBuf) { ret(ctx, 0); }
HLE_FUNC(sys_io, cellPadSetPortSetting) { ret(ctx, 0); }
HLE_FUNC(sys_io, cellPadSetActDirect) { ret(ctx, 0); }

}
