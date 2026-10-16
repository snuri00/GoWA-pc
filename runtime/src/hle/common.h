#pragma once

#include "runtime/hle.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "ppu/ops.h"

#include <cstring>
#include <string>

namespace hle {

inline uint32_t a32(ppu::Context& c, int n) { return static_cast<uint32_t>(c.gpr[3 + n]); }
inline uint64_t a64(ppu::Context& c, int n) { return c.gpr[3 + n]; }
inline int32_t s32(ppu::Context& c, int n) { return static_cast<int32_t>(c.gpr[3 + n]); }
inline void ret(ppu::Context& c, int64_t v) { c.gpr[3] = static_cast<uint64_t>(v); }

inline std::string gstr(uint32_t addr, size_t max = 1024) {
    if (!addr) return {};
    const char* p = reinterpret_cast<const char*>(vm::ptr(addr));
    return std::string(p, strnlen(p, max));
}

inline void put_str(uint32_t addr, const std::string& s, size_t cap) {
    if (!addr || !cap) return;
    size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
    std::memcpy(vm::ptr(addr), s.data(), n);
    vm::ptr(addr)[n] = 0;
}

void call_guest(ppu::Context& ctx, uint32_t opd, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0);

}
