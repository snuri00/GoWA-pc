#pragma once

#include <intrin.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace rjprof {

struct Entry {
    uint64_t self = 0;
    uint64_t incl = 0;
    uint64_t calls = 0;
};

inline Entry g_table[0x10000];
inline uint32_t g_stack_fn[256];
inline uint64_t g_stack_child[256];
inline int g_depth = 0;

struct Scope {
    uint32_t idx;
    uint64_t t0;
    explicit Scope(uint32_t pc) : idx((pc >> 2) & 0xffff), t0(__rdtsc()) {
        g_stack_fn[g_depth] = idx;
        g_stack_child[g_depth] = 0;
        g_depth++;
    }
    ~Scope() {
        uint64_t dt = __rdtsc() - t0;
        g_depth--;
        Entry& e = g_table[idx];
        e.incl += dt;
        e.self += dt - g_stack_child[g_depth];
        e.calls++;
        if (g_depth > 0) g_stack_child[g_depth - 1] += dt;
    }
};

inline void dump() {
    uint64_t total = 0;
    std::vector<uint32_t> idx;
    for (uint32_t i = 0; i < 0x10000; i++)
        if (g_table[i].calls) {
            idx.push_back(i);
            total += g_table[i].self;
        }
    if (idx.empty()) return;
    for (size_t a = 0; a < idx.size(); a++)
        for (size_t b = a + 1; b < idx.size(); b++)
            if (g_table[idx[b]].self > g_table[idx[a]].self) std::swap(idx[a], idx[b]);
    std::printf("native profile (TSC, self/incl):\n");
    for (uint32_t i : idx) {
        const Entry& e = g_table[i];
        std::printf("  %05x calls %8llu self %11llu (%5.1f%%) incl %11llu  %8.0f self/call\n", i << 2, (unsigned long long)e.calls, (unsigned long long)e.self, total ? 100.0 * e.self / total : 0.0, (unsigned long long)e.incl, double(e.self) / e.calls);
    }
}

}
