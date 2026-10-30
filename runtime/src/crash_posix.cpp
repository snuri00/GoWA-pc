#ifndef _WIN32
#include "runtime/hooks.h"
#include "runtime/host.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/thread.h"
#include "runtime/trace.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <sys/mman.h>
#include <ucontext.h>

namespace crash {

namespace {

std::string symbol_of(uintptr_t addr) {
    Dl_info info{};
    char out[512];
    if (dladdr(reinterpret_cast<void*>(addr), &info) && info.dli_sname) {
        int st = 0;
        char* dm = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &st);
        std::snprintf(out, sizeof out, "%s+0x%llx", st == 0 && dm ? dm : info.dli_sname, static_cast<unsigned long long>(addr - reinterpret_cast<uintptr_t>(info.dli_saddr)));
        std::free(dm);
        return out;
    }
    if (info.dli_fname) {
        const char* base = std::strrchr(info.dli_fname, '/');
        std::snprintf(out, sizeof out, "%s+0x%llx", base ? base + 1 : info.dli_fname, static_cast<unsigned long long>(addr - reinterpret_cast<uintptr_t>(info.dli_fbase)));
        return out;
    }
    return "?";
}

int stack_from(const ucontext_t* uc, void** pcs, int depth) {
    void* raw[64];
    int n = backtrace(raw, 64);
    uintptr_t rip = uc ? static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]) : 0;
    int start = 0;
    if (uc) {
        start = std::min(n, 2);
        for (int i = 0; i < n; i++) {
            if (reinterpret_cast<uintptr_t>(raw[i]) == rip) {
                start = i;
                break;
            }
        }
    }
    int out = 0;
    if (uc && (start >= n || reinterpret_cast<uintptr_t>(raw[start]) != rip)) pcs[out++] = reinterpret_cast<void*>(rip);
    for (int i = start; i < n && out < depth; i++) pcs[out++] = raw[i];
    return out;
}

std::string stack_string(void* const* pcs, int n) {
    std::string line;
    for (int i = 0; i < n; i++) line += " <- " + symbol_of(reinterpret_cast<uintptr_t>(pcs[i]));
    return line;
}

struct Trap {
    uint32_t addr = 0;
    uint32_t len = 4;
    bool reads = false;
    bool has_val = false;
    bool dump = false;
    bool dumped = false;
    uint32_t val = 0;
    uint64_t hits = 0;
    uint32_t logged = 0;
};

struct PendingHit {
    int trap;
    uint32_t fault;
    bool write;
    void* pcs[12];
    int npcs;
};

std::vector<Trap> g_traps;
std::unordered_map<uint32_t, host::Prot> g_trap_pages;
int g_watch_count = 0;
uint32_t g_trap_max = 50;
std::mutex g_trap_lock;
thread_local uint32_t t_step_page = 0;
thread_local std::vector<PendingHit> t_hits;

void process_hit(const PendingHit& h) {
    Trap& t = g_traps[h.trap];
    uint32_t value = vm::read32(t.len <= 4 ? t.addr : (h.fault & ~3u));
    if (t.has_val && vm::read32(t.addr) != t.val) return;
    std::lock_guard lk(g_trap_lock);
    t.hits++;
    uint32_t stack_id = 0;
    if (t.logged < g_trap_max) {
        t.logged++;
        std::string stack = stack_string(h.pcs, h.npcs);
        guest::Thread* g = guest::current();
        std::string who = g ? g->name : host::thread_name();
        LOG_INFO("trap", "trap %d %s 0x%08x = 0x%08x by %s%s", h.trap, h.write ? "write" : "read", h.fault, value, who.c_str(), t.logged == g_trap_max ? " (further hits not logged)" : "");
        LOG_INFO("trap", "  %s", stack.c_str());
        if (trace::on(trace::TRAP)) stack_id = trace::intern(stack);
    }
    if (trace::on(trace::TRAP)) trace::emit(trace::T_TRAP, uint32_t(h.trap) | (uint32_t(h.write) << 8), h.fault, value, stack_id, t.len <= 4 ? 0 : vm::read32(t.addr));
    if (t.dump && !t.dumped) {
        t.dumped = true;
        char fn[64];
        std::snprintf(fn, sizeof fn, "trap_%08x.bin", t.addr);
        trace::dump_memory(fn);
    }
}

bool trap_fault(uint32_t g, bool is_write, ucontext_t* uc) {
    if (!g_trap_pages.count(g >> 12)) return false;
    for (int i = 0; i < static_cast<int>(g_traps.size()); i++) {
        const Trap& t = g_traps[i];
        if (uint64_t(g) + 16 <= t.addr || uint64_t(g) >= uint64_t(t.addr) + t.len) continue;
        if (!is_write && !t.reads) continue;
        PendingHit h{i, g, is_write, {}, 0};
        h.npcs = stack_from(uc, h.pcs, 12);
        t_hits.push_back(h);
    }
    host::vm_protect(vm::g_base + (uint64_t(g >> 12) << 12), 4096, host::Prot::ReadWrite);
    t_step_page = (g >> 12) + 1;
    uc->uc_mcontext.gregs[REG_EFL] |= 0x100;
    return true;
}

struct sigaction g_old_segv, g_old_bus, g_old_ill, g_old_fpe;

void chain(int sig, siginfo_t* si, void* ctx) {
    struct sigaction* old = sig == SIGSEGV ? &g_old_segv : sig == SIGBUS ? &g_old_bus : sig == SIGILL ? &g_old_ill : &g_old_fpe;
    if (old->sa_flags & SA_SIGINFO) {
        if (old->sa_sigaction) return old->sa_sigaction(sig, si, ctx);
    } else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) {
        return old->sa_handler(sig);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void fault_handler(int sig, siginfo_t* si, void* ctx) {
    auto* uc = static_cast<ucontext_t*>(ctx);
    auto* fault = static_cast<uint8_t*>(si->si_addr);
    bool guest = sig == SIGSEGV && vm::g_base && fault >= vm::g_base && fault < vm::g_base + 0x100000000ull;
    bool is_write = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
    if (guest) {
        uint32_t g = static_cast<uint32_t>(fault - vm::g_base);
        if (is_write && vm::watch_fault(g, g_watch_count == 0)) return;
        if (g_watch_count && trap_fault(g, is_write, uc)) return;
    }
    static std::atomic<bool> once{false};
    if (once.exchange(true)) return chain(sig, si, ctx);
    guest::Thread* t = guest::current();
    uintptr_t rip = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
    uint64_t base = reinterpret_cast<uint64_t>(vm::g_base);
    LOG_ERROR("crash", "signal %d at %p (%s) fault %p (guest 0x%llx)%s thread %s 0x%x", sig, reinterpret_cast<void*>(rip), symbol_of(rip).c_str(), si->si_addr,
              static_cast<unsigned long long>(reinterpret_cast<uint64_t>(si->si_addr) - base), is_write ? " write" : "", t ? t->name.c_str() : host::thread_name().c_str(), t ? t->id : 0);
    void* pcs[24];
    int n = stack_from(uc, pcs, 24);
    for (int i = 0; i < n; i++) LOG_ERROR("crash", "  #%d %s", i, symbol_of(reinterpret_cast<uintptr_t>(pcs[i])).c_str());
    if (t) {
        auto& c = t->ctx;
        LOG_ERROR("crash", "  guest lr 0x%llx r1 0x%llx r2 0x%llx r3 0x%llx r4 0x%llx r5 0x%llx", static_cast<unsigned long long>(c.lr), static_cast<unsigned long long>(c.gpr[1]),
                  static_cast<unsigned long long>(c.gpr[2]), static_cast<unsigned long long>(c.gpr[3]), static_cast<unsigned long long>(c.gpr[4]), static_cast<unsigned long long>(c.gpr[5]));
    }
    if (!std::getenv("GOWA_CRASH_NO_BLACKBOX")) hooks::capture("crash");
    std::fflush(nullptr);
    chain(sig, si, ctx);
}

void trap_handler(int, siginfo_t*, void* ctx) {
    auto* uc = static_cast<ucontext_t*>(ctx);
    if (!t_step_page) return;
    uint32_t page = t_step_page - 1;
    t_step_page = 0;
    uc->uc_mcontext.gregs[REG_EFL] &= ~0x100ll;
    std::vector<PendingHit> hits;
    hits.swap(t_hits);
    for (const PendingHit& h : hits) process_hit(h);
    host::vm_protect(vm::g_base + (uint64_t(page) << 12), 4096, g_trap_pages[page]);
}

const int kSampleSig = SIGRTMIN + 2;
std::mutex g_sample_lock;
std::atomic<int> g_sample_ready{0};
void* g_sample_pcs[32];
int g_sample_depth = 0;
int g_sample_n = 0;

void sample_handler(int, siginfo_t*, void* ctx) {
    g_sample_n = stack_from(static_cast<ucontext_t*>(ctx), g_sample_pcs, g_sample_depth);
    g_sample_ready.store(1, std::memory_order_release);
}

int sample_pcs(void* native, void** pcs, int depth) {
    std::lock_guard lk(g_sample_lock);
    g_sample_depth = std::min(depth, 32);
    g_sample_n = 0;
    g_sample_ready.store(0, std::memory_order_release);
    if (pthread_kill(reinterpret_cast<pthread_t>(native), kSampleSig) != 0) return 0;
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (!g_sample_ready.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > end) return 0;
        std::this_thread::yield();
    }
    std::memcpy(pcs, g_sample_pcs, sizeof(void*) * g_sample_n);
    return g_sample_n;
}

void handle(int sig, void (*fn)(int, siginfo_t*, void*), struct sigaction* old) {
    struct sigaction sa{};
    sa.sa_sigaction = fn;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, old);
}

}

void install() {
    void* warm[4];
    backtrace(warm, 4);
    handle(SIGSEGV, fault_handler, &g_old_segv);
    handle(SIGBUS, fault_handler, &g_old_bus);
    handle(SIGILL, fault_handler, &g_old_ill);
    handle(SIGFPE, fault_handler, &g_old_fpe);
    handle(SIGTRAP, trap_handler, nullptr);
    handle(kSampleSig, sample_handler, nullptr);
}

std::string sample_stack(void* native, int depth) {
    void* pcs[32];
    int n = sample_pcs(native, pcs, std::min(depth, 32));
    return stack_string(pcs, n);
}

void profile_thread(void* native, const char* name) {
    std::thread([native, name] {
        std::map<std::string, uint64_t> counts;
        uint64_t total = 0;
        auto last = std::chrono::steady_clock::now();
        static const int depth = std::getenv("GOWA_PROFILE_DEPTH") ? std::min(16, std::atoi(std::getenv("GOWA_PROFILE_DEPTH"))) : 3;
        static const size_t top_n = std::getenv("GOWA_PROFILE_TOP") ? size_t(std::atoi(std::getenv("GOWA_PROFILE_TOP"))) : size_t(12);
        for (;;) {
            host::sleep_ms(1);
            void* pcs[16];
            int n = sample_pcs(native, pcs, depth);
            if (!n) continue;
            std::string key;
            for (int i = 0; i < n; i++) {
                std::string sym = symbol_of(reinterpret_cast<uintptr_t>(pcs[i]));
                size_t plus = sym.rfind('+');
                if (plus != std::string::npos && sym.compare(0, plus, "gowa") != 0) sym.resize(plus);
                key += (i ? " <- " : "") + sym;
            }
            counts[key]++;
            total++;
            auto now = std::chrono::steady_clock::now();
            if (now - last > std::chrono::seconds(5)) {
                last = now;
                std::vector<std::pair<uint64_t, std::string>> top;
                for (auto& [k, v] : counts) top.push_back({v, k});
                std::sort(top.rbegin(), top.rend());
                LOG_INFO("profile", "%s: %llu samples", name, static_cast<unsigned long long>(total));
                for (size_t i = 0; i < top.size() && i < top_n; i++) LOG_INFO("profile", "  %5.1f%% %s", 100.0 * top[i].first / total, top[i].second.c_str());
                counts.clear();
                total = 0;
            }
        }
    }).detach();
}

void log_here(const char* tag) {
    void* pcs[16];
    int n = stack_from(nullptr, pcs, 16);
    LOG_INFO("watch", "stack at %s", tag);
    LOG_INFO("watch", "  %s", stack_string(pcs, n).c_str());
}

void rearm_watch() {
    for (auto& [page, prot] : g_trap_pages) host::vm_protect(vm::g_base + (uint64_t(page) << 12), 4096, prot);
}

void arm_watch() {
    const char* env = std::getenv("GOWA_TRAP");
    if (!env) env = std::getenv("GOWA_WATCH");
    if (!env || g_watch_count) return;
    if (const char* m = std::getenv("GOWA_TRAP_MAX")) g_trap_max = static_cast<uint32_t>(std::atoi(m));
    std::string list = env;
    for (char& ch : list) if (ch == ';') ch = ',';
    size_t pos = 0;
    while (pos < list.size()) {
        size_t end = list.find(',', pos);
        if (end == std::string::npos) end = list.size();
        std::string item = list.substr(pos, end - pos);
        pos = end + 1;
        if (item.empty()) continue;
        Trap t;
        if (item[0] == 'r') {
            t.reads = true;
            item = item.substr(1);
        }
        size_t bang = item.find('!');
        if (bang != std::string::npos) {
            t.dump = item.substr(bang + 1) == "dump";
            item = item.substr(0, bang);
        }
        size_t eq = item.find('=');
        if (eq != std::string::npos) {
            t.has_val = true;
            t.val = static_cast<uint32_t>(std::strtoul(item.c_str() + eq + 1, nullptr, 16));
            item = item.substr(0, eq);
        }
        size_t plus = item.find('+');
        size_t dash = item.find('-');
        t.addr = static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 16));
        if (plus != std::string::npos) t.len = static_cast<uint32_t>(std::strtoul(item.c_str() + plus + 1, nullptr, 16));
        else if (dash != std::string::npos) t.len = static_cast<uint32_t>(std::strtoul(item.c_str() + dash + 1, nullptr, 16)) - t.addr;
        if (!t.len) t.len = 4;
        g_traps.push_back(t);
    }
    for (size_t i = 0; i < g_traps.size(); i++) {
        const Trap& t = g_traps[i];
        for (uint64_t p = t.addr >> 12; p <= (uint64_t(t.addr) + t.len - 1) >> 12; p++) {
            auto [it, fresh] = g_trap_pages.try_emplace(static_cast<uint32_t>(p), host::Prot::Read);
            if (t.reads) it->second = host::Prot::None;
        }
        LOG_INFO("trap", "trap %zu: %s 0x%08x+0x%x%s%s", i, t.reads ? "read/write" : "write", t.addr, t.len, t.has_val ? " value-match" : "", t.dump ? " dump" : "");
    }
    g_watch_count = static_cast<int>(g_traps.size());
    rearm_watch();
}

}
#endif
