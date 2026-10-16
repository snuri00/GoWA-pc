#include "lwsync.h"

#include <algorithm>

namespace lv2 {

namespace {

struct LwCond : Object {
    std::shared_ptr<LwMutex> mutex;
    uint32_t control = 0;
    std::list<LwWaiter*> waiters;
};

LwWaiter* take_waiter(LwCond& c, uint32_t tid) {
    if (c.waiters.empty()) return nullptr;
    if (tid == 0xFFFFFFFF) {
        LwWaiter* w = c.waiters.front();
        c.waiters.pop_front();
        return w;
    }
    auto it = std::find_if(c.waiters.begin(), c.waiters.end(), [&](LwWaiter* w) { return w->tid == tid; });
    if (it == c.waiters.end()) return nullptr;
    LwWaiter* w = *it;
    c.waiters.erase(it);
    return w;
}

}

LV2_SYSCALL(111, _sys_lwcond_create) {
    uint32_t out = u32arg(ctx, 0);
    auto m = get<LwMutex>(u32arg(ctx, 1));
    if (!m) return CELL_ESRCH;
    auto c = std::make_shared<LwCond>();
    c->mutex = m;
    c->control = u32arg(ctx, 2);
    c->name = name_u64(u64arg(ctx, 3));
    vm::write32(out, add_object(0x97000000, c));
    return CELL_OK;
}

LV2_SYSCALL(112, _sys_lwcond_destroy) {
    auto c = get<LwCond>(u32arg(ctx, 0));
    if (!c) return CELL_ESRCH;
    std::lock_guard lk(c->mutex->m);
    if (!c->waiters.empty()) return CELL_EBUSY;
    remove_object(u32arg(ctx, 0));
    return CELL_OK;
}

LV2_SYSCALL(113, _sys_lwcond_queue_wait) {
    auto c = get<LwCond>(u32arg(ctx, 0));
    if (!c) return CELL_ESRCH;
    auto& m = *c->mutex;
    uint64_t timeout = u64arg(ctx, 2);
    std::unique_lock lk(m.m);
    LwWaiter w{current_thread_id()};
    c->waiters.push_back(&w);
    m.lwcond_waiters++;
    m.release(CELL_OK);
    bool ok = wait_for(lk, m.cv, timeout, [&] { return w.woken; });
    m.lwcond_waiters--;
    if (!ok) {
        c->waiters.remove(&w);
        m.sleepers.remove(&w);
        return CELL_ETIMEDOUT;
    }
    return w.result;
}

LV2_SYSCALL(115, _sys_lwcond_signal) {
    auto c = get<LwCond>(u32arg(ctx, 0));
    if (!c) return CELL_ESRCH;
    uint32_t tid = static_cast<uint32_t>(u64arg(ctx, 2));
    uint32_t mode = u32arg(ctx, 3);
    auto& m = *c->mutex;
    std::lock_guard lk(m.m);
    LwWaiter* w = take_waiter(*c, tid);
    if (!w) {
        if (tid != 0xFFFFFFFF) return CELL_EPERM;
        return mode == 1 ? CELL_EPERM : mode == 2 ? CELL_OK : CELL_ENOENT;
    }
    if (mode == 1) {
        m.sleepers.push_back(w);
    } else {
        w->woken = true;
        w->result = mode == 2 ? CELL_EBUSY : CELL_OK;
        m.cv.notify_all();
    }
    return CELL_OK;
}

LV2_SYSCALL(116, _sys_lwcond_signal_all) {
    auto c = get<LwCond>(u32arg(ctx, 0));
    if (!c) return CELL_ESRCH;
    uint32_t mode = u32arg(ctx, 2);
    auto& m = *c->mutex;
    std::lock_guard lk(m.m);
    int64_t moved = 0;
    while (LwWaiter* w = take_waiter(*c, 0xFFFFFFFF)) {
        if (mode == 1) {
            m.sleepers.push_back(w);
            moved++;
        } else {
            w->woken = true;
            w->result = CELL_EBUSY;
        }
    }
    m.cv.notify_all();
    return mode == 1 ? moved : CELL_OK;
}

}
