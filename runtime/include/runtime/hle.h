#pragma once

#include "ppu/context.h"

#include <string>

namespace hle {

using Fn = void (*)(ppu::Context& ctx, uint8_t* base);

struct Reg {
    Reg(const char* module, const char* name, Fn fn);
};

Fn find(const char* module, const char* name);

using Tap = void (*)(const uint64_t* args, uint64_t result);

struct TapReg {
    TapReg(const char* module, const char* name, Tap tap);
};

Tap find_tap(const char* module, const char* name);

struct ModuleOverride {
    ModuleOverride(const char* file, bool (*active)());
};

bool module_overridden(const std::string& file);

}

#define HLE_FUNC(module, name) \
    static void hle_##module##_##name(ppu::Context& ctx, uint8_t* base); \
    static hle::Reg hreg_##module##_##name(#module, #name, hle_##module##_##name); \
    static void hle_##module##_##name(ppu::Context& ctx, uint8_t* base)
