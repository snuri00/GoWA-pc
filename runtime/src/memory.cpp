#include "runtime/memory.h"
#include "runtime/log.h"

#include <map>
#include <mutex>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace vm {

uint8_t* g_base = nullptr;

namespace {

struct RegionInfo {
    uint32_t start;
    uint32_t size;
    uint32_t page;
};

constexpr RegionInfo kRegions[] = {
    {0x00010000, 0x1FFF0000, 0x10000},
    {0x20000000, 0x10000000, 0x10000},
    {0x30000000, 0x10000000, 0x100000},
    {0x40000000, 0x10000000, 0x100000},
    {0xC0000000, 0x10000000, 0x100000},
    {0xD0000000, 0x10000000, 0x10000},
    {0xE0000000, 0x20000000, 0x10000},
};

std::mutex g_lock;
std::map<uint32_t, uint32_t> g_blocks;
uint32_t g_used[static_cast<size_t>(Region::Count)] = {};

bool commit(uint32_t addr, uint32_t size) {
    return VirtualAlloc(g_base + addr, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

bool overlaps(uint32_t addr, uint32_t size) {
    auto it = g_blocks.upper_bound(addr);
    if (it != g_blocks.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second > addr) return true;
    }
    return it != g_blocks.end() && it->first < addr + size;
}

}

bool init() {
    void* p = VirtualAlloc(nullptr, 0x200000000ull, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) {
        LOG_ERROR("vm", "failed to reserve guest address space");
        return false;
    }
    g_base = static_cast<uint8_t*>(p);
    LOG_INFO("vm", "guest base %p", p);
    return true;
}

bool map(uint32_t addr, uint32_t size) {
    std::lock_guard lk(g_lock);
    uint32_t a = addr & ~0xFFFFu;
    uint32_t e = (addr + size + 0xFFFF) & ~0xFFFFu;
    if (!commit(a, e - a)) return false;
    g_blocks[a] = e - a;
    return true;
}

uint32_t alloc(Region region, uint32_t size, uint32_t align) {
    std::lock_guard lk(g_lock);
    const RegionInfo& r = kRegions[static_cast<size_t>(region)];
    if (align < r.page) align = r.page;
    size = (size + r.page - 1) & ~(r.page - 1);
    const uint64_t end = uint64_t(r.start) + r.size;
    uint64_t a = (uint64_t(r.start) + align - 1) & ~uint64_t(align - 1);
    while (a + size <= end) {
        auto it = g_blocks.upper_bound(uint32_t(a + size - 1));
        if (it != g_blocks.begin()) {
            auto prev = std::prev(it);
            uint64_t prev_end = uint64_t(prev->first) + prev->second;
            if (prev_end > a) {
                a = (prev_end + align - 1) & ~uint64_t(align - 1);
                continue;
            }
        }
        if (!commit(uint32_t(a), size)) return 0;
        std::memset(g_base + a, 0, size);
        g_blocks[uint32_t(a)] = size;
        g_used[static_cast<size_t>(region)] += size;
        return uint32_t(a);
    }
    return 0;
}

bool dealloc(uint32_t addr) {
    std::lock_guard lk(g_lock);
    auto it = g_blocks.find(addr);
    if (it == g_blocks.end()) return false;
    for (size_t i = 0; i < static_cast<size_t>(Region::Count); i++) {
        if (addr >= kRegions[i].start && addr < kRegions[i].start + kRegions[i].size) {
            g_used[i] -= it->second;
        }
    }
    VirtualFree(g_base + addr, it->second, MEM_DECOMMIT);
    g_blocks.erase(it);
    return true;
}

uint32_t region_free(Region region) {
    std::lock_guard lk(g_lock);
    return kRegions[static_cast<size_t>(region)].size - g_used[static_cast<size_t>(region)];
}

uint32_t region_size(Region region) { return kRegions[static_cast<size_t>(region)].size; }

bool is_mapped(uint32_t addr) {
    std::lock_guard lk(g_lock);
    auto it = g_blocks.upper_bound(addr);
    if (it == g_blocks.begin()) return false;
    auto prev = std::prev(it);
    return addr < prev->first + prev->second;
}

}
