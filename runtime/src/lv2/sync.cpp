#include "object.h"

#include <algorithm>
#include <list>

namespace lv2 {

namespace {

struct Mutex : Object {
    std::mutex m;
    std::condition_variable cv;
    uint32_t owner = 0;
    uint32_t count = 0;
    uint32_t cond_count = 0;
    bool recursive = false;
    uint32_t protocol = 0;
};

struct CondWaiter {
    uint32_t tid;
    bool woken = false;
};

struct Cond : Object {
    std::shared_ptr<Mutex> mutex;
    std::condition_variable cv;
    std::list<CondWaiter*> waiters;
};

struct RwLock : Object {
    std::mutex m;
    std::condition_variable cv;
    uint32_t readers = 0;
    uint32_t writer = 0;
    uint32_t waiting_writers = 0;
};

struct Semaphore : Object {
    std::mutex m;
    std::condition_variable cv;
    int32_t value = 0;
    int32_t max = 0;
};

struct EventFlag : Object {
    std::mutex m;
    std::condition_variable cv;
    uint64_t pattern = 0;
    int32_t type = 1;
    uint32_t waiters = 0;
    uint64_t cancel_gen = 0;
};

bool flag_match(uint64_t pattern, uint64_t bits, uint32_t mode) {
    return (mode & 0xF) == 1 ? (pattern & bits) == bits : (pattern & bits) != 0;
}

void flag_consume(uint64_t& pattern, uint64_t bits, uint32_t mode) {
    if (mode & 0x10) pattern &= ~bits;
    else if (mode & 0x20) pattern = 0;
}

}

LV2_SYSCALL(100, sys_mutex_create) {
    uint32_t out = u32arg(ctx, 0);
    uint32_t attr = u32arg(ctx, 1);
    if (!out || !attr) return CELL_EFAULT;
    auto m = std::make_shared<Mutex>();
    m->protocol = vm::read32(attr);
    m->recursive = vm::read32(attr + 4) == 0x10;
    m->name = name8(attr + 32);
    uint32_t id = add_object(0x85000000, m);
    vm::write32(out, id);
    LOG_DEBUG("sync", "sys_mutex_create '%s' -> 0x%x", m->name.c_str(), id);
    return CELL_OK;
}

LV2_SYSCALL(101, sys_mutex_destroy) {
    auto m = get<Mutex>(u32arg(ctx, 0));
    if (!m) return CELL_ESRCH;
    std::lock_guard lk(m->m);
    if (m->owner || m->cond_count) return CELL_EBUSY;
    remove_object(u32arg(ctx, 0));
    return CELL_OK;
}

LV2_SYSCALL(102, sys_mutex_lock) {
    auto m = get<Mutex>(u32arg(ctx, 0));
    if (!m) return CELL_ESRCH;
    uint32_t me = current_thread_id();
    std::unique_lock lk(m->m);
    if (m->owner == me) {
        if (!m->recursive) return CELL_EDEADLK;
        m->count++;
        return CELL_OK;
    }
    if (!wait_for(lk, m->cv, u64arg(ctx, 1), [&] { return m->owner == 0; })) return CELL_ETIMEDOUT;
    m->owner = me;
    m->count = 1;
    return CELL_OK;
}

LV2_SYSCALL(103, sys_mutex_trylock) {
    auto m = get<Mutex>(u32arg(ctx, 0));
    if (!m) return CELL_ESRCH;
    uint32_t me = current_thread_id();
    std::lock_guard lk(m->m);
    if (m->owner == me) {
        if (!m->recursive) return CELL_EDEADLK;
        m->count++;
        return CELL_OK;
    }
    if (m->owner) return CELL_EBUSY;
    m->owner = me;
    m->count = 1;
    return CELL_OK;
}

LV2_SYSCALL(104, sys_mutex_unlock) {
    auto m = get<Mutex>(u32arg(ctx, 0));
    if (!m) return CELL_ESRCH;
    std::lock_guard lk(m->m);
    if (m->owner != current_thread_id()) return CELL_EPERM;
    if (--m->count == 0) {
        m->owner = 0;
        m->cv.notify_all();
    }
    return CELL_OK;
}

LV2_SYSCALL(105, sys_cond_create) {
    uint32_t out = u32arg(ctx, 0);
    auto m = get<Mutex>(u32arg(ctx, 1));
    if (!m) return CELL_ESRCH;
    auto c = std::make_shared<Cond>();
    c->mutex = m;
    if (uint32_t attr = u32arg(ctx, 2)) c->name = name8(attr + 16);
    {
        std::lock_guard lk(m->m);
        m->cond_count++;
    }
    vm::write32(out, add_object(0x86000000, c));
    return CELL_OK;
}

LV2_SYSCALL(106, sys_cond_destroy) {
    auto c = get<Cond>(u32arg(ctx, 0));
    if (!c) return CELL_ESRCH;
    std::lock_guard lk(c->mutex->m);
    if (!c->waiters.empty()) return CELL_EBUSY;
    c->mutex->cond_count--;
    remove_object(u32arg(ctx, 0));
    return CELL_OK;
}

LV2_SYSCALL(107, sys_cond_wait) {
    auto c = get<Cond>(u32arg(ctx, 0));
    if (!c) return CELL_ESRCH;
    auto& m = *c->mutex;
    uint32_t me = current_thread_id();
    std::unique_lock lk(m.m);
    if (m.owner != me) return CELL_EPERM;
    uint32_t count = m.count;
    m.owner = 0;
    m.count = 0;
    m.cv.notify_all();
    CondWaiter w{me};
    c->waiters.push_back(&w);
    bool ok = wait_for(lk, c->cv, u64arg(ctx, 1), [&] { return w.woken; });
    if (!ok) c->waiters.remove(&w);
    m.cv.wait(lk, [&] { return m.owner == 0; });
    m.owner = me;
    m.count = count;
    return ok ? CELL_OK : CELL_ETIMEDOUT;
}

LV2_SYSCALL(108, sys_cond_signal) {
    auto c = get<Cond>(u32arg(ctx, 0));
    if (!c) return CELL_ESRCH;
    std::lock_guard lk(c->mutex->m);
    if (!c->waiters.empty()) {
        c->waiters.front()->woken = true;
        c->waiters.pop_front();
        c->cv.notify_all();
    }
    return CELL_OK;
}

LV2_SYSCALL(109, sys_cond_signal_all) {
    auto c = get<Cond>(u32arg(ctx, 0));
    if (!c) return CELL_ESRCH;
    std::lock_guard lk(c->mutex->m);
    for (auto* w : c->waiters) w->woken = true;
    c->waiters.clear();
    c->cv.notify_all();
    return CELL_OK;
}

LV2_SYSCALL(110, sys_cond_signal_to) {
    auto c = get<Cond>(u32arg(ctx, 0));
    if (!c) return CELL_ESRCH;
    uint32_t tid = u32arg(ctx, 1);
    std::lock_guard lk(c->mutex->m);
    for (auto it = c->waiters.begin(); it != c->waiters.end(); ++it) {
        if ((*it)->tid == tid) {
            (*it)->woken = true;
            c->waiters.erase(it);
            c->cv.notify_all();
            return CELL_OK;
        }
    }
    return CELL_EPERM;
}

LV2_SYSCALL(120, sys_rwlock_create) {
    auto r = std::make_shared<RwLock>();
    if (uint32_t attr = u32arg(ctx, 1)) r->name = name8(attr + 24);
    vm::write32(u32arg(ctx, 0), add_object(0x88000000, r));
    return CELL_OK;
}

LV2_SYSCALL(121, sys_rwlock_destroy) {
    auto r = get<RwLock>(u32arg(ctx, 0));
    if (!r) return CELL_ESRCH;
    std::lock_guard lk(r->m);
    if (r->readers || r->writer) return CELL_EBUSY;
    remove_object(u32arg(ctx, 0));
    return CELL_OK;
}

LV2_SYSCALL(122, sys_rwlock_rlock) {
    auto r = get<RwLock>(u32arg(ctx, 0));
    if (!r) return CELL_ESRCH;
    std::unique_lock lk(r->m);
    if (r->writer == current_thread_id()) return CELL_EDEADLK;
    if (!wait_for(lk, r->cv, u64arg(ctx, 1), [&] { return !r->writer && !r->waiting_writers; })) return CELL_ETIMEDOUT;
    r->readers++;
    return CELL_OK;
}

LV2_SYSCALL(123, sys_rwlock_tryrlock) {
    auto r = get<RwLock>(u32arg(ctx, 0));
    if (!r) return CELL_ESRCH;
    std::lock_guard lk(r->m);
    if (r->writer || r->waiting_writers) return CELL_EBUSY;
    r->readers++;
    return CELL_OK;
}

LV2_SYSCALL(124, sys_rwlock_runlock) {
    auto r = get<RwLock>(u32arg(ctx, 0));
    if (!r) return CELL_ESRCH;
    std::lock_guard lk(r->m);
    if (!r->readers) return CELL_EPERM;
    if (--r->readers == 0) r->cv.notify_all();
    return CELL_OK;
}

LV2_SYSCALL(125, sys_rwlock_wlock) {
    auto r = get<RwLock>(u32arg(ctx, 0));
    if (!r) return CELL_ESRCH;
    uint32_t me = current_thread_id();
    std::unique_lock lk(r->m);
    if (r->writer == me) return CELL_EDEADLK;
    r->waiting_writers++;
    bool ok = wait_for(lk, r->cv, u64arg(ctx, 1), [&] { return !r->writer && !r->readers; });
    r->waiting_writers--;
    if (!ok) {
        r->cv.notify_all();
        return CELL_ETIMEDOUT;
    }
    r->writer = me;
    return CELL_OK;
}

LV2_SYSCALL(126, sys_rwlock_trywlock) {
    auto r = get<RwLock>(u32arg(ctx, 0));
    if (!r) return CELL_ESRCH;
    uint32_t me = current_thread_id();
    std::lock_guard lk(r->m);
    if (r->writer == me) return CELL_EDEADLK;
    if (r->writer || r->readers) return CELL_EBUSY;
    r->writer = me;
    return CELL_OK;
}

LV2_SYSCALL(148, _sys_rwlock_trywlock) {
    return sys_rwlock_trywlock(ctx, base);
}

LV2_SYSCALL(127, sys_rwlock_wunlock) {
    auto r = get<RwLock>(u32arg(ctx, 0));
    if (!r) return CELL_ESRCH;
    std::lock_guard lk(r->m);
    if (r->writer != current_thread_id()) return CELL_EPERM;
    r->writer = 0;
    r->cv.notify_all();
    return CELL_OK;
}

LV2_SYSCALL(90, sys_semaphore_create) {
    uint32_t out = u32arg(ctx, 0);
    uint32_t attr = u32arg(ctx, 1);
    int32_t init = s32arg(ctx, 2);
    int32_t max = s32arg(ctx, 3);
    if (max <= 0 || init < 0 || init > max) return CELL_EINVAL;
    auto s = std::make_shared<Semaphore>();
    s->value = init;
    s->max = max;
    if (attr) s->name = name8(attr + 24);
    vm::write32(out, add_object(0x96000000, s));
    return CELL_OK;
}

LV2_SYSCALL(91, sys_semaphore_destroy) {
    if (!get<Semaphore>(u32arg(ctx, 0))) return CELL_ESRCH;
    remove_object(u32arg(ctx, 0));
    return CELL_OK;
}

LV2_SYSCALL(92, sys_semaphore_wait) {
    auto s = get<Semaphore>(u32arg(ctx, 0));
    if (!s) return CELL_ESRCH;
    std::unique_lock lk(s->m);
    if (!wait_for(lk, s->cv, u64arg(ctx, 1), [&] { return s->value > 0; })) return CELL_ETIMEDOUT;
    s->value--;
    return CELL_OK;
}

LV2_SYSCALL(93, sys_semaphore_trywait) {
    auto s = get<Semaphore>(u32arg(ctx, 0));
    if (!s) return CELL_ESRCH;
    std::lock_guard lk(s->m);
    if (s->value <= 0) return CELL_EBUSY;
    s->value--;
    return CELL_OK;
}

LV2_SYSCALL(94, sys_semaphore_post) {
    auto s = get<Semaphore>(u32arg(ctx, 0));
    if (!s) return CELL_ESRCH;
    int32_t n = s32arg(ctx, 1);
    if (n < 0) return CELL_EINVAL;
    std::lock_guard lk(s->m);
    if (s->value + n > s->max) return CELL_EBUSY;
    s->value += n;
    s->cv.notify_all();
    return CELL_OK;
}

LV2_SYSCALL(114, sys_semaphore_get_value) {
    auto s = get<Semaphore>(u32arg(ctx, 0));
    if (!s) return CELL_ESRCH;
    std::lock_guard lk(s->m);
    vm::write32(u32arg(ctx, 1), static_cast<uint32_t>(s->value));
    return CELL_OK;
}

LV2_SYSCALL(82, sys_event_flag_create) {
    uint32_t out = u32arg(ctx, 0);
    uint32_t attr = u32arg(ctx, 1);
    auto f = std::make_shared<EventFlag>();
    f->pattern = u64arg(ctx, 2);
    if (attr) {
        f->type = static_cast<int32_t>(vm::read32(attr + 20));
        f->name = name8(attr + 24);
    }
    vm::write32(out, add_object(0x98000000, f));
    return CELL_OK;
}

LV2_SYSCALL(83, sys_event_flag_destroy) {
    auto f = get<EventFlag>(u32arg(ctx, 0));
    if (!f) return CELL_ESRCH;
    std::lock_guard lk(f->m);
    if (f->waiters) return CELL_EBUSY;
    remove_object(u32arg(ctx, 0));
    return CELL_OK;
}

LV2_SYSCALL(85, sys_event_flag_wait) {
    auto f = get<EventFlag>(u32arg(ctx, 0));
    if (!f) return CELL_ESRCH;
    uint64_t bits = u64arg(ctx, 1);
    uint32_t mode = u32arg(ctx, 2);
    uint32_t result = u32arg(ctx, 3);
    std::unique_lock lk(f->m);
    if (f->type == 1 && f->waiters) return CELL_EPERM;
    uint64_t gen = f->cancel_gen;
    f->waiters++;
    bool ok = wait_for(lk, f->cv, u64arg(ctx, 4), [&] { return flag_match(f->pattern, bits, mode) || f->cancel_gen != gen; });
    f->waiters--;
    if (result) vm::write64(result, f->pattern);
    if (f->cancel_gen != gen) return CELL_ECANCELED;
    if (!ok) return CELL_ETIMEDOUT;
    flag_consume(f->pattern, bits, mode);
    return CELL_OK;
}

LV2_SYSCALL(86, sys_event_flag_trywait) {
    auto f = get<EventFlag>(u32arg(ctx, 0));
    if (!f) return CELL_ESRCH;
    uint64_t bits = u64arg(ctx, 1);
    uint32_t mode = u32arg(ctx, 2);
    std::lock_guard lk(f->m);
    if (uint32_t result = u32arg(ctx, 3)) vm::write64(result, f->pattern);
    if (!flag_match(f->pattern, bits, mode)) return CELL_EBUSY;
    flag_consume(f->pattern, bits, mode);
    return CELL_OK;
}

LV2_SYSCALL(87, sys_event_flag_set) {
    auto f = get<EventFlag>(u32arg(ctx, 0));
    if (!f) return CELL_ESRCH;
    std::lock_guard lk(f->m);
    f->pattern |= u64arg(ctx, 1);
    f->cv.notify_all();
    return CELL_OK;
}

LV2_SYSCALL(118, sys_event_flag_clear) {
    auto f = get<EventFlag>(u32arg(ctx, 0));
    if (!f) return CELL_ESRCH;
    std::lock_guard lk(f->m);
    f->pattern &= u64arg(ctx, 1);
    return CELL_OK;
}

LV2_SYSCALL(132, sys_event_flag_cancel) {
    auto f = get<EventFlag>(u32arg(ctx, 0));
    if (!f) return CELL_ESRCH;
    std::lock_guard lk(f->m);
    if (uint32_t num = u32arg(ctx, 1)) vm::write32(num, f->waiters);
    f->cancel_gen++;
    f->cv.notify_all();
    return CELL_OK;
}

LV2_SYSCALL(139, sys_event_flag_get) {
    auto f = get<EventFlag>(u32arg(ctx, 0));
    if (!f) return CELL_ESRCH;
    std::lock_guard lk(f->m);
    vm::write64(u32arg(ctx, 1), f->pattern);
    return CELL_OK;
}

}
