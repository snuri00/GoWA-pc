#pragma once

#include <cstdint>
#include <cstring>

namespace vm {

enum class Region : uint32_t {
    Main,
    User64K,
    User1M,
    RsxContext,
    Video,
    Stack,
    Spu,
    Count,
};

extern uint8_t* g_base;

bool init();
bool map(uint32_t addr, uint32_t size);
uint32_t alloc(Region region, uint32_t size, uint32_t align);
uint32_t reserve_slot(uint32_t size, uint32_t align);
void release_slot(uint32_t addr);
bool dealloc(uint32_t addr);
uint32_t region_free(Region region);
uint32_t region_size(Region region);
bool is_mapped(uint32_t addr);

inline uint8_t* ptr(uint32_t addr) { return g_base + addr; }

inline uint32_t read32(uint32_t addr) { uint32_t v; std::memcpy(&v, g_base + addr, 4); return __builtin_bswap32(v); }
inline uint64_t read64(uint32_t addr) { uint64_t v; std::memcpy(&v, g_base + addr, 8); return __builtin_bswap64(v); }
inline uint16_t read16(uint32_t addr) { uint16_t v; std::memcpy(&v, g_base + addr, 2); return __builtin_bswap16(v); }
inline void write32(uint32_t addr, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(g_base + addr, &v, 4); }
inline void write64(uint32_t addr, uint64_t v) { v = __builtin_bswap64(v); std::memcpy(g_base + addr, &v, 8); }
inline void write16(uint32_t addr, uint16_t v) { v = __builtin_bswap16(v); std::memcpy(g_base + addr, &v, 2); }

}
