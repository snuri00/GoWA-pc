#pragma once

#include "ppu/context.h"

namespace hle {

using Fn = void (*)(ppu::Context& ctx, uint8_t* base);

struct Reg {
    Reg(const char* module, const char* name, Fn fn);
};

Fn find(const char* module, const char* name);

}

#define HLE_FUNC(module, name) \
    static void hle_##module##_##name(ppu::Context& ctx, uint8_t* base); \
    static hle::Reg hreg_##module##_##name(#module, #name, hle_##module##_##name); \
    static void hle_##module##_##name(ppu::Context& ctx, uint8_t* base)
