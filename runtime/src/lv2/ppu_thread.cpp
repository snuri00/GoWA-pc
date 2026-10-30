#include "object.h"

#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace lv2 {

LV2_SYSCALL(41, _sys_ppu_thread_exit) {
    throw ThreadExit{u64arg(ctx, 0)};
}

LV2_SYSCALL(43, sys_ppu_thread_yield) {
    std::this_thread::yield();
    return CELL_OK;
}

LV2_SYSCALL(44, sys_ppu_thread_join) {
    uint32_t id = u32arg(ctx, 0);
    uint32_t vptr = u32arg(ctx, 1);
    if (id == current_thread_id()) return CELL_EDEADLK;
    auto t = guest::find(id);
    if (!t) return CELL_ESRCH;
    std::unique_lock lk(t->lock);
    if (!t->joinable || t->joined) return CELL_EINVAL;
    t->joined = true;
    t->cv.wait(lk, [&] { return t->exited; });
    if (vptr) vm::write64(vptr, t->exit_value);
    lk.unlock();
    guest::remove(id);
    return CELL_OK;
}

LV2_SYSCALL(45, sys_ppu_thread_detach) {
    auto t = guest::find(u32arg(ctx, 0));
    if (!t) return CELL_ESRCH;
    std::lock_guard lk(t->lock);
    if (!t->joinable) return CELL_EINVAL;
    if (t->joined) return CELL_EBUSY;
    t->joinable = false;
    return CELL_OK;
}

LV2_SYSCALL(46, sys_ppu_thread_get_join_state) {
    guest::Thread* t = guest::current();
    vm::write32(u32arg(ctx, 0), t && t->joinable ? 1 : 0);
    return CELL_OK;
}

LV2_SYSCALL(47, sys_ppu_thread_set_priority) {
    auto t = guest::find(u32arg(ctx, 0));
    if (!t) return CELL_ESRCH;
    t->prio = s32arg(ctx, 1);
    return CELL_OK;
}

LV2_SYSCALL(48, sys_ppu_thread_get_priority) {
    auto t = guest::find(u32arg(ctx, 0));
    if (!t) return CELL_ESRCH;
    vm::write32(u32arg(ctx, 1), static_cast<uint32_t>(t->prio));
    return CELL_OK;
}

LV2_SYSCALL(49, sys_ppu_thread_get_stack_information) {
    guest::Thread* t = guest::current();
    uint32_t out = u32arg(ctx, 0);
    vm::write32(out, t->stack_addr);
    vm::write32(out + 4, t->stack_size);
    return CELL_OK;
}

LV2_SYSCALL(52, _sys_ppu_thread_create) {
    uint32_t out = u32arg(ctx, 0);
    uint32_t param = u32arg(ctx, 1);
    uint64_t arg = u64arg(ctx, 2);
    uint64_t unk = u64arg(ctx, 3);
    int32_t prio = s32arg(ctx, 4);
    uint32_t stack = u32arg(ctx, 5);
    uint64_t flags = u64arg(ctx, 6);
    uint32_t namep = u32arg(ctx, 7);
    if (!param) return CELL_EFAULT;
    uint32_t entry = vm::read32(param);
    uint32_t tls = vm::read32(param + 4);
    if (!entry) return CELL_EFAULT;
    if (prio < 0 || prio > 3071) return CELL_EINVAL;
    if ((flags & 3) == 3) return CELL_EPERM;
    std::string name = namep ? reinterpret_cast<const char*>(vm::ptr(namep)) : "";
    auto t = guest::create(name, entry, arg, unk, prio, stack ? stack : 0x4000, tls, (flags & 1) != 0);
    if (!t) return CELL_ENOMEM;
    vm::write64(out, t->id);
    LOG_INFO("ppu", "_sys_ppu_thread_create '%s' entry 0x%08x arg 0x%llx prio %d stack 0x%x flags 0x%llx -> 0x%x", name.c_str(), entry,
             static_cast<unsigned long long>(arg), prio, stack, static_cast<unsigned long long>(flags), t->id);
    return CELL_OK;
}

LV2_SYSCALL(53, sys_ppu_thread_start) {
    auto t = guest::find(u32arg(ctx, 0));
    if (!t) return CELL_ESRCH;
    guest::start(t);
    return CELL_OK;
}

LV2_SYSCALL(56, sys_ppu_thread_rename) {
    auto t = guest::find(u32arg(ctx, 0));
    if (!t) return CELL_ESRCH;
    if (uint32_t p = u32arg(ctx, 1)) t->name = reinterpret_cast<const char*>(vm::ptr(p));
    return CELL_OK;
}

}
