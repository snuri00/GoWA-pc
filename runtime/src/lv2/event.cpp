#include "event.h"

namespace lv2 {

namespace {

struct EventPort : Object {
    std::mutex m;
    int32_t type = 1;
    uint64_t port_name = 0;
    uint32_t queue = 0;
};

std::mutex g_keys_lock;
std::map<uint64_t, uint32_t> g_keys;

}

int64_t push_event(const std::shared_ptr<EventQueue>& q, const Event& e) {
    std::lock_guard lk(q->m);
    if (q->destroyed) return CELL_ESRCH;
    if (q->events.size() >= q->size) return CELL_EBUSY;
    q->events.push_back(e);
    q->cv.notify_one();
    return CELL_OK;
}

std::shared_ptr<EventQueue> queue_by_key(uint64_t key) {
    std::lock_guard lk(g_keys_lock);
    auto it = g_keys.find(key);
    return it == g_keys.end() ? nullptr : get<EventQueue>(it->second);
}

LV2_SYSCALL(128, sys_event_queue_create) {
    uint32_t out = u32arg(ctx, 0);
    uint32_t attr = u32arg(ctx, 1);
    uint64_t key = u64arg(ctx, 2);
    int32_t size = s32arg(ctx, 3);
    if (size <= 0 || size > 127) return CELL_EINVAL;
    auto q = std::make_shared<EventQueue>();
    q->size = static_cast<uint32_t>(size);
    q->key = key;
    if (attr) {
        q->type = static_cast<int32_t>(vm::read32(attr + 4));
        q->name = name8(attr + 8);
    }
    uint32_t id = add_object(0x8D000000, q);
    if (key) {
        std::lock_guard lk(g_keys_lock);
        if (g_keys.count(key)) {
            remove_object(id);
            return CELL_EEXIST;
        }
        g_keys[key] = id;
    }
    vm::write32(out, id);
    LOG_DEBUG("event", "sys_event_queue_create '%s' key 0x%llx size %d -> 0x%x", q->name.c_str(), static_cast<unsigned long long>(key), size, id);
    return CELL_OK;
}

LV2_SYSCALL(129, sys_event_queue_destroy) {
    uint32_t id = u32arg(ctx, 0);
    int32_t mode = s32arg(ctx, 1);
    auto q = get<EventQueue>(id);
    if (!q) return CELL_ESRCH;
    {
        std::lock_guard lk(q->m);
        if (q->waiters && mode != 1) return CELL_EBUSY;
        q->destroyed = true;
        q->cv.notify_all();
    }
    if (q->key) {
        std::lock_guard lk(g_keys_lock);
        g_keys.erase(q->key);
    }
    remove_object(id);
    return CELL_OK;
}

LV2_SYSCALL(130, sys_event_queue_receive) {
    auto q = get<EventQueue>(u32arg(ctx, 0));
    if (!q) return CELL_ESRCH;
    if (q->type != 1) return CELL_EINVAL;
    std::unique_lock lk(q->m);
    q->waiters++;
    bool ok = wait_for(lk, q->cv, u64arg(ctx, 2), [&] { return !q->events.empty() || q->destroyed; });
    q->waiters--;
    if (q->destroyed) return CELL_ECANCELED;
    if (!ok) return CELL_ETIMEDOUT;
    Event e = q->events.front();
    q->events.pop_front();
    ctx.gpr[4] = e.source;
    ctx.gpr[5] = e.data1;
    ctx.gpr[6] = e.data2;
    ctx.gpr[7] = e.data3;
    return CELL_OK;
}

LV2_SYSCALL(131, sys_event_queue_tryreceive) {
    auto q = get<EventQueue>(u32arg(ctx, 0));
    if (!q) return CELL_ESRCH;
    uint32_t arr = u32arg(ctx, 1);
    int32_t size = s32arg(ctx, 2);
    uint32_t number = u32arg(ctx, 3);
    std::lock_guard lk(q->m);
    int32_t n = 0;
    while (n < size && !q->events.empty()) {
        const Event& e = q->events.front();
        vm::write64(arr + n * 32, e.source);
        vm::write64(arr + n * 32 + 8, e.data1);
        vm::write64(arr + n * 32 + 16, e.data2);
        vm::write64(arr + n * 32 + 24, e.data3);
        q->events.pop_front();
        n++;
    }
    vm::write32(number, static_cast<uint32_t>(n));
    return CELL_OK;
}

LV2_SYSCALL(133, sys_event_queue_drain) {
    auto q = get<EventQueue>(u32arg(ctx, 0));
    if (!q) return CELL_ESRCH;
    std::lock_guard lk(q->m);
    q->events.clear();
    return CELL_OK;
}

LV2_SYSCALL(134, sys_event_port_create) {
    uint32_t out = u32arg(ctx, 0);
    auto p = std::make_shared<EventPort>();
    p->type = s32arg(ctx, 1);
    p->port_name = u64arg(ctx, 2);
    vm::write32(out, add_object(0x0E000000, p));
    return CELL_OK;
}

LV2_SYSCALL(135, sys_event_port_destroy) {
    auto p = get<EventPort>(u32arg(ctx, 0));
    if (!p) return CELL_ESRCH;
    if (p->queue) return CELL_EISCONN;
    remove_object(u32arg(ctx, 0));
    return CELL_OK;
}

LV2_SYSCALL(136, sys_event_port_connect_local) {
    auto p = get<EventPort>(u32arg(ctx, 0));
    if (!p) return CELL_ESRCH;
    if (!get<EventQueue>(u32arg(ctx, 1))) return CELL_ESRCH;
    std::lock_guard lk(p->m);
    if (p->queue) return CELL_EISCONN;
    p->queue = u32arg(ctx, 1);
    return CELL_OK;
}

LV2_SYSCALL(140, sys_event_port_connect_ipc) {
    auto p = get<EventPort>(u32arg(ctx, 0));
    if (!p) return CELL_ESRCH;
    auto q = queue_by_key(u64arg(ctx, 1));
    if (!q) return CELL_ESRCH;
    std::lock_guard lk(p->m);
    if (p->queue) return CELL_EISCONN;
    std::lock_guard kl(g_keys_lock);
    p->queue = g_keys[u64arg(ctx, 1)];
    return CELL_OK;
}

LV2_SYSCALL(137, sys_event_port_disconnect) {
    auto p = get<EventPort>(u32arg(ctx, 0));
    if (!p) return CELL_ESRCH;
    std::lock_guard lk(p->m);
    if (!p->queue) return CELL_ENOTCONN;
    p->queue = 0;
    return CELL_OK;
}

LV2_SYSCALL(138, sys_event_port_send) {
    uint32_t id = u32arg(ctx, 0);
    auto p = get<EventPort>(id);
    if (!p) return CELL_ESRCH;
    uint32_t qid;
    {
        std::lock_guard lk(p->m);
        qid = p->queue;
    }
    if (!qid) return CELL_ENOTCONN;
    auto q = get<EventQueue>(qid);
    if (!q) return CELL_ENOTCONN;
    uint64_t source = p->port_name ? p->port_name : (uint64_t(0x01000500) << 32) | id;
    return push_event(q, {source, u64arg(ctx, 1), u64arg(ctx, 2), u64arg(ctx, 3)});
}

}
