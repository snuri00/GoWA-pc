#include "event.h"
#include "ppu/ops.h"

#include <thread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace lv2 {

namespace {

struct Timer : Object {
    uint32_t queue = 0;
    uint64_t source = 0;
    uint64_t data1 = 0;
    uint64_t data2 = 0;
    uint64_t next = 0;
    uint64_t period = 0;
    bool running = false;
};

std::mutex g_lock;
std::condition_variable g_cv;
std::map<uint32_t, std::shared_ptr<Timer>> g_timers;
bool g_thread_started = false;

uint64_t now_us() { return ppu::timebase() / 80; }

void timer_thread() {
    SetThreadDescription(GetCurrentThread(), L"LV2 Timer");
    std::unique_lock lk(g_lock);
    for (;;) {
        uint64_t now = now_us();
        uint64_t soonest = ~0ull;
        for (auto& [id, t] : g_timers) {
            if (!t->running) continue;
            if (t->next <= now) {
                uint64_t expire = t->next;
                if (auto q = get<EventQueue>(t->queue)) push_event(q, {t->source, t->data1, t->data2, expire});
                if (t->period) {
                    t->next += t->period;
                    if (t->next <= now) t->next = now + t->period;
                } else {
                    t->running = false;
                    continue;
                }
            }
            soonest = std::min(soonest, t->next);
        }
        if (soonest == ~0ull) g_cv.wait_for(lk, std::chrono::milliseconds(100));
        else g_cv.wait_for(lk, std::chrono::microseconds(static_cast<uint64_t>((soonest > now ? soonest - now : 0) / ppu::time_scale())));
    }
}

void ensure_thread() {
    if (!g_thread_started) {
        g_thread_started = true;
        std::thread(timer_thread).detach();
    }
}

}

LV2_SYSCALL(70, sys_timer_create) {
    auto t = std::make_shared<Timer>();
    uint32_t id = add_object(0x11000000, t);
    {
        std::lock_guard lk(g_lock);
        g_timers[id] = t;
        ensure_thread();
    }
    vm::write32(u32arg(ctx, 0), id);
    return CELL_OK;
}

LV2_SYSCALL(71, sys_timer_destroy) {
    uint32_t id = u32arg(ctx, 0);
    std::lock_guard lk(g_lock);
    auto it = g_timers.find(id);
    if (it == g_timers.end()) return CELL_ESRCH;
    if (it->second->queue) return CELL_EISCONN;
    g_timers.erase(it);
    remove_object(id);
    return CELL_OK;
}

LV2_SYSCALL(72, sys_timer_get_information) {
    std::lock_guard lk(g_lock);
    auto it = g_timers.find(u32arg(ctx, 0));
    if (it == g_timers.end()) return CELL_ESRCH;
    uint32_t out = u32arg(ctx, 1);
    auto& t = *it->second;
    vm::write64(out, t.running ? t.next : 0);
    vm::write64(out + 8, t.running ? t.period : 0);
    vm::write32(out + 16, t.running ? 1 : 0);
    vm::write32(out + 20, 0);
    return CELL_OK;
}

LV2_SYSCALL(73, _sys_timer_start) {
    uint64_t base_time = u64arg(ctx, 1), period = u64arg(ctx, 2);
    if (period && period < 100) return CELL_EINVAL;
    std::lock_guard lk(g_lock);
    auto it = g_timers.find(u32arg(ctx, 0));
    if (it == g_timers.end()) return CELL_ESRCH;
    auto& t = *it->second;
    if (!t.queue) return CELL_ENOTCONN;
    if (t.running) return CELL_EBUSY;
    uint64_t now = now_us();
    if (!period) {
        if (base_time <= now) return CELL_ETIMEDOUT;
        t.next = base_time;
    } else if (!base_time) {
        t.next = now + period;
    } else if (now < base_time + period) {
        t.next = base_time + period;
    } else {
        t.next = base_time + ((now - base_time) / period + 1) * period;
    }
    t.period = period;
    t.running = true;
    g_cv.notify_all();
    return CELL_OK;
}

LV2_SYSCALL(74, sys_timer_stop) {
    std::lock_guard lk(g_lock);
    auto it = g_timers.find(u32arg(ctx, 0));
    if (it == g_timers.end()) return CELL_ESRCH;
    it->second->running = false;
    return CELL_OK;
}

LV2_SYSCALL(75, sys_timer_connect_event_queue) {
    uint32_t id = u32arg(ctx, 0), q = u32arg(ctx, 1);
    if (!get<EventQueue>(q)) return CELL_ESRCH;
    std::lock_guard lk(g_lock);
    auto it = g_timers.find(id);
    if (it == g_timers.end()) return CELL_ESRCH;
    auto& t = *it->second;
    if (t.queue) return CELL_EISCONN;
    t.queue = q;
    uint64_t name = u64arg(ctx, 2);
    t.source = name ? name : ((uint64_t(1) << 32) | id);
    t.data1 = u64arg(ctx, 3);
    t.data2 = u64arg(ctx, 4);
    return CELL_OK;
}

LV2_SYSCALL(76, sys_timer_disconnect_event_queue) {
    std::lock_guard lk(g_lock);
    auto it = g_timers.find(u32arg(ctx, 0));
    if (it == g_timers.end()) return CELL_ESRCH;
    if (!it->second->queue) return CELL_ENOTCONN;
    it->second->queue = 0;
    it->second->running = false;
    return CELL_OK;
}

}
