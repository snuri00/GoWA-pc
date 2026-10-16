#include "object.h"

#include <vector>

namespace lv2 {

namespace {

struct Area {
    uint32_t addr;
    uint32_t size;
    uint32_t page;
};

struct SharedMem : Object {
    uint32_t size = 0;
    uint32_t page = 0x100000;
    uint32_t mapped_at = 0;
    std::vector<uint8_t> saved;
};

std::mutex g_lock;
std::map<uint32_t, Area> g_areas;
std::map<uint32_t, uint32_t> g_maps;

bool area_free(uint32_t addr, uint32_t size) {
    for (auto& [a, ar] : g_areas) {
        if (addr < a + ar.size && a < addr + size) return false;
    }
    return true;
}

const Area* area_of(uint32_t addr) {
    for (auto& [a, ar] : g_areas) {
        if (addr >= a && addr < a + ar.size) return &ar;
    }
    return nullptr;
}

}

LV2_SYSCALL(330, sys_mmapper_allocate_address) {
    uint64_t size = u64arg(ctx, 0);
    uint64_t flags = u64arg(ctx, 1);
    uint64_t align = u64arg(ctx, 2);
    uint32_t out = u32arg(ctx, 3);
    if (!size || size % 0x10000000) return CELL_EALIGN;
    if (!align) align = 0x10000000;
    std::lock_guard lk(g_lock);
    for (uint64_t a = 0x50000000; a + size <= 0xC0000000; a += align) {
        if (area_free(static_cast<uint32_t>(a), static_cast<uint32_t>(size))) {
            g_areas[static_cast<uint32_t>(a)] = {static_cast<uint32_t>(a), static_cast<uint32_t>(size), (flags & 0xF00) == 0x200 ? 0x10000u : 0x100000u};
            vm::write32(out, static_cast<uint32_t>(a));
            LOG_INFO("mmapper", "allocate_address 0x%llx -> 0x%08x", static_cast<unsigned long long>(size), static_cast<uint32_t>(a));
            return CELL_OK;
        }
    }
    return CELL_ENOMEM;
}

LV2_SYSCALL(331, sys_mmapper_free_address) {
    std::lock_guard lk(g_lock);
    return g_areas.erase(u32arg(ctx, 0)) ? CELL_OK : CELL_EINVAL;
}

LV2_SYSCALL(332, sys_mmapper_allocate_shared_memory) {
    uint64_t size = u64arg(ctx, 1);
    uint64_t flags = u64arg(ctx, 2);
    uint32_t out = u32arg(ctx, 3);
    uint32_t page = (flags & 0xF00) == 0x200 ? 0x10000 : 0x100000;
    if (!size || size % page) return CELL_EALIGN;
    auto m = std::make_shared<SharedMem>();
    m->size = static_cast<uint32_t>(size);
    m->page = page;
    vm::write32(out, add_object(0x08000000, m));
    return CELL_OK;
}

LV2_SYSCALL(362, sys_mmapper_allocate_shared_memory_from_container) {
    uint64_t size = u64arg(ctx, 1);
    uint64_t flags = u64arg(ctx, 3);
    uint32_t out = u32arg(ctx, 4);
    uint32_t page = (flags & 0xF00) == 0x200 ? 0x10000 : 0x100000;
    if (!size || size % page) return CELL_EALIGN;
    auto m = std::make_shared<SharedMem>();
    m->size = static_cast<uint32_t>(size);
    m->page = page;
    vm::write32(out, add_object(0x08000000, m));
    return CELL_OK;
}

LV2_SYSCALL(329, sys_mmapper_free_shared_memory) {
    auto m = get<SharedMem>(u32arg(ctx, 0));
    if (!m) return CELL_ESRCH;
    if (m->mapped_at) return CELL_EBUSY;
    remove_object(u32arg(ctx, 0));
    return CELL_OK;
}

LV2_SYSCALL(334, sys_mmapper_map_shared_memory) {
    uint32_t addr = u32arg(ctx, 0);
    uint32_t id = u32arg(ctx, 1);
    auto m = get<SharedMem>(id);
    if (!m) return CELL_ESRCH;
    std::lock_guard lk(g_lock);
    const Area* ar = area_of(addr);
    if (!ar || addr + m->size > ar->addr + ar->size) return CELL_EINVAL;
    if (addr % m->page) return CELL_EALIGN;
    if (m->mapped_at) return CELL_EBUSY;
    if (!vm::map(addr, m->size)) return CELL_ENOMEM;
    if (!m->saved.empty()) std::memcpy(vm::ptr(addr), m->saved.data(), m->size);
    else std::memset(vm::ptr(addr), 0, m->size);
    m->mapped_at = addr;
    g_maps[addr] = id;
    return CELL_OK;
}

LV2_SYSCALL(337, sys_mmapper_search_and_map) {
    uint32_t start = u32arg(ctx, 0);
    uint32_t id = u32arg(ctx, 1);
    uint32_t out = u32arg(ctx, 3);
    auto m = get<SharedMem>(id);
    if (!m) return CELL_ESRCH;
    std::lock_guard lk(g_lock);
    const Area* ar = area_of(start);
    if (!ar) return CELL_EINVAL;
    for (uint32_t a = ar->addr; a + m->size <= ar->addr + ar->size; a += m->page) {
        bool used = false;
        for (auto& [ma, mid] : g_maps) {
            auto mm = get<SharedMem>(mid);
            if (mm && a < ma + mm->size && ma < a + m->size) {
                used = true;
                break;
            }
        }
        if (used) continue;
        if (!vm::map(a, m->size)) return CELL_ENOMEM;
        std::memset(vm::ptr(a), 0, m->size);
        m->mapped_at = a;
        g_maps[a] = id;
        vm::write32(out, a);
        return CELL_OK;
    }
    return CELL_ENOMEM;
}

LV2_SYSCALL(335, sys_mmapper_unmap_shared_memory) {
    uint32_t addr = u32arg(ctx, 0);
    uint32_t out = u32arg(ctx, 1);
    std::lock_guard lk(g_lock);
    auto it = g_maps.find(addr);
    if (it == g_maps.end()) return CELL_EINVAL;
    auto m = get<SharedMem>(it->second);
    if (m) {
        m->saved.assign(vm::ptr(addr), vm::ptr(addr) + m->size);
        m->mapped_at = 0;
        vm::dealloc(addr);
    }
    if (out) vm::write32(out, it->second);
    g_maps.erase(it);
    return CELL_OK;
}

}
