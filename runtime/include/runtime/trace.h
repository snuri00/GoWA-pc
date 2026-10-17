#pragma once

#include <cstdint>
#include <string>

namespace trace {

enum Category : uint32_t {
    SYSCALL = 1u << 0,
    IMPORT = 1u << 1,
    FS = 1u << 2,
    RSX = 1u << 3,
    TEX = 1u << 4,
    DMA = 1u << 5,
    TRAP = 1u << 6,
    DRAW = 1u << 7,
    ATOMIC = 1u << 8,
};

enum Type : uint16_t {
    T_THREAD = 1,
    T_SYSCALL = 2,
    T_IMPORT = 3,
    T_FS_OPEN = 4,
    T_FS_READ = 5,
    T_FS_WRITE = 6,
    T_COPY = 7,
    T_SEMA = 8,
    T_REPORT = 9,
    T_FLIP = 10,
    T_TEX = 11,
    T_DMA = 12,
    T_TRAP = 13,
    T_DRAW = 14,
    T_MARK = 15,
    T_ATOMIC = 16,
    T_FS_CLOSE = 17,
};

enum CopyEngine : uint32_t {
    COPY_NV3089 = 1,
    COPY_NV0039 = 2,
    COPY_NV308A = 3,
};

extern uint32_t g_mask;

inline bool on(uint32_t cat) { return (g_mask & cat) != 0; }

void init();
void shutdown();
void flush_all();
void emit(uint16_t type, uint32_t x, uint64_t a, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0, uint64_t e = 0);
uint32_t intern(const std::string& s);
uint64_t now_ns();
bool dump_memory(const char* path);

}
