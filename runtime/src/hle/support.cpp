#include "common.h"
#include "runtime/thread.h"

#include <map>

namespace hle {

namespace {

std::map<std::string, bool (*)()>& overrides() {
    static std::map<std::string, bool (*)()> m;
    return m;
}

}

ModuleOverride::ModuleOverride(const char* file, bool (*active)()) { overrides()[file] = active; }

bool module_overridden(const std::string& file) {
    auto it = overrides().find(file);
    return it != overrides().end() && it->second();
}

void call_guest(ppu::Context& ctx, uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    ppu::Context saved = ctx;
    ctx.gpr[1] = (ctx.gpr[1] - 0x200) & ~0xFull;
    ctx.gpr[3] = a0;
    ctx.gpr[4] = a1;
    ctx.gpr[5] = a2;
    ctx.gpr[6] = a3;
    guest::call_opd(ctx, vm::g_base, opd);
    uint64_t r3 = ctx.gpr[3];
    ctx = saved;
    ctx.gpr[3] = r3;
}

}
