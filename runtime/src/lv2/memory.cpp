#include "object.h"

namespace lv2 {

namespace {

constexpr uint32_t kUserMemory = 0x0D500000;
std::mutex g_lock;
uint32_t g_used = 0;
std::map<uint32_t, uint32_t> g_allocs;

struct Container : Object {
    uint32_t size = 0;
    uint32_t used = 0;
};

}

LV2_SYSCALL(348, sys_memory_allocate) {
    uint32_t size = u32arg(ctx, 0);
    uint64_t flags = u64arg(ctx, 1);
    uint32_t out = u32arg(ctx, 2);
    uint32_t page = flags == 0x200 ? 0x10000 : 0x100000;
    if (!size || size % page) return CELL_EALIGN;
    if (!out) return CELL_EFAULT;
    std::lock_guard lk(g_lock);
    if (g_used + size > kUserMemory) return CELL_ENOMEM;
    uint32_t addr = vm::alloc(page == 0x10000 ? vm::Region::User64K : vm::Region::User1M, size, page);
    if (!addr) return CELL_ENOMEM;
    g_used += size;
    g_allocs[addr] = size;
    vm::write32(out, addr);
    LOG_DEBUG("memory", "sys_memory_allocate(0x%x, 0x%llx) -> 0x%08x", size, static_cast<unsigned long long>(flags), addr);
    return CELL_OK;
}

LV2_SYSCALL(349, sys_memory_free) {
    uint32_t addr = u32arg(ctx, 0);
    std::lock_guard lk(g_lock);
    auto it = g_allocs.find(addr);
    if (it == g_allocs.end()) return CELL_EINVAL;
    g_used -= it->second;
    g_allocs.erase(it);
    vm::dealloc(addr);
    return CELL_OK;
}

LV2_SYSCALL(350, sys_memory_allocate_from_container) {
    uint32_t size = u32arg(ctx, 0);
    uint64_t flags = u64arg(ctx, 2);
    uint32_t out = u32arg(ctx, 3);
    uint32_t page = flags == 0x200 ? 0x10000 : 0x100000;
    if (!size || size % page) return CELL_EALIGN;
    uint32_t addr = vm::alloc(page == 0x10000 ? vm::Region::User64K : vm::Region::User1M, size, page);
    if (!addr) return CELL_ENOMEM;
    std::lock_guard lk(g_lock);
    g_allocs[addr] = 0;
    vm::write32(out, addr);
    return CELL_OK;
}

LV2_SYSCALL(351, sys_memory_get_page_attribute) {
    uint32_t addr = u32arg(ctx, 0);
    uint32_t out = u32arg(ctx, 1);
    if (!vm::is_mapped(addr)) return CELL_EINVAL;
    vm::write64(out, 0x40000ull);
    vm::write64(out + 8, 0xFull);
    vm::write32(out + 16, addr >= 0x30000000 && addr < 0x40000000 ? 0x100000 : 0x10000);
    vm::write32(out + 20, 0);
    return CELL_OK;
}

LV2_SYSCALL(352, sys_memory_get_user_memory_size) {
    uint32_t out = u32arg(ctx, 0);
    std::lock_guard lk(g_lock);
    vm::write32(out, kUserMemory);
    vm::write32(out + 4, kUserMemory - g_used);
    return CELL_OK;
}

LV2_SYSCALL(324, sys_memory_container_create) {
    uint32_t out = u32arg(ctx, 0);
    auto c = std::make_shared<Container>();
    c->size = u32arg(ctx, 1);
    std::lock_guard lk(g_lock);
    if (g_used + c->size > kUserMemory) return CELL_ENOMEM;
    g_used += c->size;
    vm::write32(out, add_object(0x3F000000, c));
    return CELL_OK;
}

LV2_SYSCALL(341, sys_memory_container_create2) {
    return sys_memory_container_create(ctx, base);
}

LV2_SYSCALL(325, sys_memory_container_destroy) {
    auto c = get<Container>(u32arg(ctx, 0));
    if (!c) return CELL_ESRCH;
    std::lock_guard lk(g_lock);
    g_used -= c->size;
    remove_object(u32arg(ctx, 0));
    return CELL_OK;
}

LV2_SYSCALL(342, sys_memory_container_destroy2) {
    return sys_memory_container_destroy(ctx, base);
}

LV2_SYSCALL(343, sys_memory_container_get_size) {
    uint32_t out = u32arg(ctx, 0);
    uint32_t cid = u32arg(ctx, 1);
    std::lock_guard lk(g_lock);
    if (cid == 0xFFFFFFFF || cid == 0) {
        vm::write32(out, kUserMemory);
        vm::write32(out + 4, kUserMemory - g_used);
        return CELL_OK;
    }
    auto c = get<Container>(cid);
    if (!c) return CELL_ESRCH;
    vm::write32(out, c->size);
    vm::write32(out + 4, c->size - c->used);
    return CELL_OK;
}

}
