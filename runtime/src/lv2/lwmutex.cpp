#include "lwsync.h"

namespace lv2 {

int64_t LwMutex::sleep(std::unique_lock<std::mutex>& lk, LwWaiter& w, uint64_t timeout) {
    sleepers.push_back(&w);
    bool ok = wait_for(lk, cv, timeout, [&] { return w.woken; });
    if (!ok) {
        sleepers.remove(&w);
        return CELL_ETIMEDOUT;
    }
    return w.result;
}

void LwMutex::release(int64_t result) {
    if (sleepers.empty()) {
        signaled = result == CELL_OK ? 1 : 2;
        return;
    }
    LwWaiter* w = sleepers.front();
    sleepers.pop_front();
    w->woken = true;
    w->result = result;
    cv.notify_all();
}

LV2_SYSCALL(95, _sys_lwmutex_create) {
    uint32_t out = u32arg(ctx, 0);
    auto m = std::make_shared<LwMutex>();
    m->protocol = u32arg(ctx, 1);
    m->control = u32arg(ctx, 2);
    if (s32arg(ctx, 3)) m->name = name_u64(u64arg(ctx, 4));
    vm::write32(out, add_object(0x95000000, m));
    return CELL_OK;
}

LV2_SYSCALL(96, _sys_lwmutex_destroy) {
    auto m = get<LwMutex>(u32arg(ctx, 0));
    if (!m) return CELL_ESRCH;
    std::lock_guard lk(m->m);
    if (!m->sleepers.empty() || m->lwcond_waiters) return CELL_EBUSY;
    remove_object(u32arg(ctx, 0));
    return CELL_OK;
}

LV2_SYSCALL(97, _sys_lwmutex_lock) {
    auto m = get<LwMutex>(u32arg(ctx, 0));
    if (!m) return CELL_ESRCH;
    std::unique_lock lk(m->m);
    if (int s = m->signaled) {
        m->signaled = 0;
        return (s & 1) ? CELL_OK : CELL_EBUSY;
    }
    LwWaiter w{current_thread_id()};
    return m->sleep(lk, w, u64arg(ctx, 1));
}

LV2_SYSCALL(99, _sys_lwmutex_trylock) {
    auto m = get<LwMutex>(u32arg(ctx, 0));
    if (!m) return CELL_ESRCH;
    std::lock_guard lk(m->m);
    if (m->signaled & 1) {
        m->signaled = 0;
        return CELL_OK;
    }
    return CELL_EBUSY;
}

LV2_SYSCALL(98, _sys_lwmutex_unlock) {
    auto m = get<LwMutex>(u32arg(ctx, 0));
    if (!m) return CELL_ESRCH;
    std::lock_guard lk(m->m);
    m->release(CELL_OK);
    return CELL_OK;
}

LV2_SYSCALL(117, _sys_lwmutex_unlock2) {
    auto m = get<LwMutex>(u32arg(ctx, 0));
    if (!m) return CELL_ESRCH;
    std::lock_guard lk(m->m);
    m->release(CELL_EBUSY);
    return CELL_OK;
}

}
