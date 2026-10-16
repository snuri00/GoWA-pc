#include "runtime/thread.h"
#include "runtime/kernel.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "ppu/ops.h"

#include <map>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace crash {
std::string sample_stack(void* native, int depth);
}

namespace lv2 {
void lwmutex_debug(uint32_t id);
}

namespace guest {

namespace {

std::mutex g_lock;
std::map<uint32_t, std::shared_ptr<Thread>> g_threads;
uint32_t g_next_id = 0x01000000;
thread_local Thread* t_current = nullptr;

void set_host_name(const std::string& name) {
    wchar_t w[128];
    int n = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, w, 128);
    if (n > 0) SetThreadDescription(GetCurrentThread(), w);
}

void body(std::shared_ptr<Thread> t) {
    t_current = t.get();
    t->native = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
    set_host_name("PPU " + t->name);
    LOG_INFO("ppu", "thread 0x%x '%s' start entry opd 0x%08x", t->id, t->name.c_str(), t->entry_opd);
    uint64_t status = 0;
    try {
        call_opd(t->ctx, vm::g_base, t->entry_opd);
        status = t->ctx.gpr[3];
    } catch (const lv2::ThreadExit& e) {
        status = e.status;
    } catch (const lv2::ProcessExit& e) {
        LOG_INFO("ppu", "process exit %d from thread 0x%x", e.status, t->id);
        std::fflush(nullptr);
        std::quick_exit(e.status);
    }
    LOG_INFO("ppu", "thread 0x%x '%s' exit 0x%llx", t->id, t->name.c_str(), static_cast<unsigned long long>(status));
    {
        std::lock_guard lk(t->lock);
        t->exited = true;
        t->exit_value = status;
    }
    t->cv.notify_all();
}

}

Thread* current() { return t_current; }

void status_dump() {
    std::lock_guard lk(g_lock);
    for (auto& [id, t] : g_threads) {
        if (t->exited) continue;
        uint64_t n = t->syscalls.load();
        uint32_t sc = t->in_syscall.load();
        LOG_INFO("status", "ppu 0x%x '%s' %s%u (r3 0x%llx r31 0x%llx) syscalls +%llu lr 0x%llx", id, t->name.c_str(), sc ? "blocked in " : "running, last ", sc ? sc - 1 : 0,
                 static_cast<unsigned long long>(t->ctx.gpr[3]), static_cast<unsigned long long>(t->ctx.gpr[31]), static_cast<unsigned long long>(n - t->syscalls_seen), static_cast<unsigned long long>(t->ctx.lr));
        t->syscalls_seen = n;
        if (!sc) {
            auto& g = t->ctx.gpr;
            LOG_INFO("status", "   regs r3 %llx r4 %llx r5 %llx r6 %llx r7 %llx r9 %llx r10 %llx r11 %llx", (unsigned long long)g[3], (unsigned long long)g[4], (unsigned long long)g[5],
                     (unsigned long long)g[6], (unsigned long long)g[7], (unsigned long long)g[9], (unsigned long long)g[10], (unsigned long long)g[11]);
        }
        if (sc == 98) lv2::lwmutex_debug(static_cast<uint32_t>(t->ctx.gpr[3]));
        if ((!sc || sc == 142 || t->name == "main_thread") && t->native) LOG_INFO("status", "   %s", crash::sample_stack(t->native, 14).c_str());
    }
}

std::shared_ptr<Thread> find(uint32_t id) {
    std::lock_guard lk(g_lock);
    auto it = g_threads.find(id);
    return it == g_threads.end() ? nullptr : it->second;
}

std::shared_ptr<Thread> create(const std::string& name, uint32_t entry_opd, uint64_t arg0, uint64_t arg1, int32_t prio, uint32_t stack_size, uint32_t tls, bool joinable) {
    stack_size = (stack_size + 0xFFF) & ~0xFFFu;
    if (stack_size < 0x4000) stack_size = 0x4000;
    uint32_t stack = vm::alloc(vm::Region::Stack, stack_size, 0x1000);
    if (!stack) return nullptr;
    auto t = std::make_shared<Thread>();
    t->name = name;
    t->entry_opd = entry_opd;
    t->stack_addr = stack;
    t->stack_size = stack_size;
    t->prio = prio;
    t->joinable = joinable;
    t->ctx.gpr[1] = stack + stack_size - 0x70;
    t->ctx.gpr[13] = tls;
    t->ctx.gpr[3] = arg0;
    t->ctx.gpr[4] = arg1;
    t->ctx.vscr_nj = 1;
    {
        std::lock_guard lk(g_lock);
        t->id = g_next_id++;
        g_threads[t->id] = t;
    }
    t->ctx.thread_id = t->id;
    return t;
}

void start(const std::shared_ptr<Thread>& t) {
    std::lock_guard lk(t->lock);
    if (t->started) return;
    t->started = true;
    t->host = std::thread(body, t);
    t->host.detach();
}

void remove(uint32_t id) {
    std::lock_guard lk(g_lock);
    g_threads.erase(id);
}

void call_opd(ppu::Context& ctx, uint8_t* base, uint32_t opd) {
    uint32_t code = vm::read32(opd);
    ctx.gpr[2] = vm::read32(opd + 4);
    ctx.lr = 0;
    ppu::call_indirect(ctx, base, code);
}

}
