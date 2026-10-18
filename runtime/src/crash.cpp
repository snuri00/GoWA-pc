#include "runtime/hooks.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/thread.h"
#include "runtime/trace.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <thread>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <chrono>
#include <windows.h>
#include <dbghelp.h>

namespace crash {

namespace {

std::string symbol_of(DWORD64 addr) {
    static std::mutex sym_lock;
    std::lock_guard lk(sym_lock);
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto* s = reinterpret_cast<SYMBOL_INFO*>(buf);
    s->SizeOfStruct = sizeof(SYMBOL_INFO);
    s->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (SymFromAddr(GetCurrentProcess(), addr, &disp, s)) {
        char out[320];
        std::snprintf(out, sizeof out, "%s+0x%llx", s->Name, static_cast<unsigned long long>(disp));
        return out;
    }
    return "?";
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
    uint32_t burst_tid = 0;
    bool burst_write = false;
    uint32_t burst_lo = 0;
    uint32_t burst_hi = 0;
    uint32_t burst_n = 0;
    uint64_t burst_t = 0;
};

struct PendingHit {
    int trap;
    uint32_t fault;
    bool write;
    CONTEXT ctx;
};

std::vector<Trap> g_traps;
std::unordered_map<uint32_t, DWORD> g_trap_pages;
int g_watch_count = 0;
uint32_t g_trap_max = 50;
std::mutex g_trap_lock;
thread_local uint32_t t_step_page = 0;
thread_local std::vector<PendingHit> t_hits;

std::string stack_string(CONTEXT ctx, int depth) {
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    std::string line;
    for (int i = 0; i < depth; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &frame, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
        line += " <- " + symbol_of(frame.AddrPC.Offset);
    }
    return line;
}

void log_stack(CONTEXT ctx, int depth) { LOG_INFO("watch", "  %s", stack_string(ctx, depth).c_str()); }

std::string current_thread_name() {
    if (guest::Thread* t = guest::current()) return t->name;
    PWSTR w = nullptr;
    std::string name = "host";
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &w)) && w) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            name.resize(n - 1);
            WideCharToMultiByte(CP_UTF8, 0, w, -1, name.data(), n, nullptr, nullptr);
        }
        LocalFree(w);
    }
    return name;
}

void close_burst(int i) {
    Trap& t = g_traps[i];
    if (t.burst_n > 1) {
        LOG_INFO("trap", "trap %d %s burst 0x%08x-0x%08x %u accesses", i, t.burst_write ? "write" : "read", t.burst_lo, t.burst_hi, t.burst_n);
        if (trace::on(trace::TRAP)) trace::emit(trace::T_TRAP, uint32_t(i) | (uint32_t(t.burst_write) << 8) | 0x10000u, t.burst_lo, t.burst_hi, 0, t.burst_n);
    }
    t.burst_n = 0;
}

void process_hit(const PendingHit& h) {
    Trap& t = g_traps[h.trap];
    uint32_t word = t.len <= 4 ? t.addr : (h.fault & ~3u);
    uint32_t value = vm::read32(word);
    if (t.has_val && vm::read32(t.addr) != t.val) return;
    uint32_t tid = GetCurrentThreadId();
    uint64_t now = trace::now_ns();
    std::lock_guard lk(g_trap_lock);
    t.hits++;
    static const bool no_burst = std::getenv("GOWA_TRAP_NOBURST") != nullptr;
    if (!no_burst && t.burst_n && t.burst_tid == tid && t.burst_write == h.write && now - t.burst_t < 2000000 && h.fault + 64 >= t.burst_lo && h.fault <= t.burst_hi + 64) {
        t.burst_lo = (std::min)(t.burst_lo, h.fault);
        t.burst_hi = (std::max)(t.burst_hi, h.fault);
        t.burst_n++;
        t.burst_t = now;
        return;
    }
    close_burst(h.trap);
    t.burst_tid = tid;
    t.burst_write = h.write;
    t.burst_lo = t.burst_hi = h.fault;
    t.burst_n = 1;
    t.burst_t = now;
    uint32_t stack_id = 0;
    if (t.logged < g_trap_max) {
        t.logged++;
        std::string stack = stack_string(h.ctx, 10);
        std::string who = current_thread_name();
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

LONG watch_handler(EXCEPTION_POINTERS* ep) {
    auto* rec = ep->ExceptionRecord;
    if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP && t_step_page) {
        uint32_t page = t_step_page - 1;
        t_step_page = 0;
        std::vector<PendingHit> hits;
        hits.swap(t_hits);
        for (const PendingHit& h : hits) process_hit(h);
        DWORD old;
        VirtualProtect(vm::g_base + (uint64_t(page) << 12), 4096, g_trap_pages[page], &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    bool is_write = rec->ExceptionInformation[0] == 1;
    auto* fault = reinterpret_cast<uint8_t*>(rec->ExceptionInformation[1]);
    if (fault < vm::g_base || fault >= vm::g_base + 0x100000000ull) return EXCEPTION_CONTINUE_SEARCH;
    uint32_t g = static_cast<uint32_t>(fault - vm::g_base);
    auto it = g_trap_pages.find(g >> 12);
    if (it == g_trap_pages.end()) return EXCEPTION_CONTINUE_SEARCH;
    for (int i = 0; i < static_cast<int>(g_traps.size()); i++) {
        const Trap& t = g_traps[i];
        if (uint64_t(g) + 16 <= t.addr || uint64_t(g) >= uint64_t(t.addr) + t.len) continue;
        if (!is_write && !t.reads) continue;
        t_hits.push_back({i, g, is_write, *ep->ContextRecord});
    }
    DWORD old;
    VirtualProtect(vm::g_base + (uint64_t(g >> 12) << 12), 4096, PAGE_READWRITE, &old);
    t_step_page = (g >> 12) + 1;
    ep->ContextRecord->EFlags |= 0x100;
    return EXCEPTION_CONTINUE_EXECUTION;
}

thread_local bool t_sampling = false;

LONG WINAPI handler(EXCEPTION_POINTERS* ep) {
    if (t_sampling) return EXCEPTION_CONTINUE_SEARCH;
    if (g_watch_count) {
        LONG r = watch_handler(ep);
        if (r == EXCEPTION_CONTINUE_EXECUTION) return r;
    }
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    static std::atomic<bool> once{false};
    if (once.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;
    auto* rec = ep->ExceptionRecord;
    uint64_t fault = rec->NumberParameters >= 2 ? rec->ExceptionInformation[1] : 0;
    uint64_t base = reinterpret_cast<uint64_t>(vm::g_base);
    guest::Thread* t = guest::current();
    LOG_ERROR("crash", "exception 0x%08lx at %p (%s) fault 0x%llx (guest 0x%llx) thread %s 0x%x", code, rec->ExceptionAddress,
              symbol_of(reinterpret_cast<DWORD64>(rec->ExceptionAddress)).c_str(), static_cast<unsigned long long>(fault),
              static_cast<unsigned long long>(fault - base), t ? t->name.c_str() : "host", t ? t->id : 0);
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 24; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &frame, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
        LOG_ERROR("crash", "  #%d %s", i, symbol_of(frame.AddrPC.Offset).c_str());
    }
    if (t) {
        auto& c = t->ctx;
        LOG_ERROR("crash", "  guest lr 0x%llx r1 0x%llx r2 0x%llx r3 0x%llx r4 0x%llx r5 0x%llx", static_cast<unsigned long long>(c.lr),
                  static_cast<unsigned long long>(c.gpr[1]), static_cast<unsigned long long>(c.gpr[2]), static_cast<unsigned long long>(c.gpr[3]),
                  static_cast<unsigned long long>(c.gpr[4]), static_cast<unsigned long long>(c.gpr[5]));
    }
    if (!std::getenv("GOWA_CRASH_NO_BLACKBOX")) hooks::capture("crash");
    return EXCEPTION_CONTINUE_SEARCH;
}

}

void install() {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    AddVectoredExceptionHandler(1, handler);
}

std::string sample_stack(void* native, int depth) {
    HANDLE h = static_cast<HANDLE>(native);
    if (SuspendThread(h) == static_cast<DWORD>(-1)) return {};
    CONTEXT c{};
    c.ContextFlags = CONTEXT_FULL;
    std::string line;
    if (GetThreadContext(h, &c)) {
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = c.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = c.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = c.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        for (int i = 0; i < depth; i++) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), h, &frame, &c, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
            line += " <- " + symbol_of(frame.AddrPC.Offset);
        }
    }
    ResumeThread(h);
    return line;
}

int sample_stack(CONTEXT& c, DWORD64* pcs, int depth) {
    int n = 0;
    t_sampling = true;
    __try {
        for (; n < depth; n++) {
            pcs[n] = c.Rip;
            if (n + 1 >= depth) {
                t_sampling = false;
                return n + 1;
            }
            DWORD64 image = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
            if (!fn) {
                t_sampling = false;
                return n + 1;
            }
            PVOID handler_data = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, fn, &c, &handler_data, &frame, nullptr);
            if (!c.Rip) {
                t_sampling = false;
                return n + 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        n++;
    }
    t_sampling = false;
    return n;
}

void profile_thread(void* native, const char* name) {
    std::thread([native, name] {
        HANDLE h = static_cast<HANDLE>(native);
        std::map<std::string, uint64_t> counts;
        uint64_t total = 0;
        auto last = std::chrono::steady_clock::now();
        for (;;) {
            Sleep(1);
            if (SuspendThread(h) == static_cast<DWORD>(-1)) return;
            CONTEXT c{};
            c.ContextFlags = CONTEXT_FULL;
            static const int depth = std::getenv("GOWA_PROFILE_DEPTH") ? (std::min)(16, std::atoi(std::getenv("GOWA_PROFILE_DEPTH"))) : 3;
            DWORD64 pcs[16] = {};
            int n = GetThreadContext(h, &c) ? sample_stack(c, pcs, depth) : 0;
            ResumeThread(h);
            std::string key;
            for (int i = 0; i < n; i++) {
                static const bool by_rip = std::getenv("GOWA_PROFILE_RIP") != nullptr;
                std::string sym = symbol_of(pcs[i]);
                size_t plus = sym.rfind('+');
                if (plus != std::string::npos && !(by_rip && i == 0)) sym.resize(plus);
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
                for (size_t i = 0; i < top.size() && i < (std::getenv("GOWA_PROFILE_TOP") ? size_t(std::atoi(std::getenv("GOWA_PROFILE_TOP"))) : size_t(12)); i++) LOG_INFO("profile", "  %5.1f%% %s", 100.0 * top[i].first / total, top[i].second.c_str());
                counts.clear();
                total = 0;
            }
        }
    }).detach();
}

void log_here(const char* tag) {
    CONTEXT c;
    RtlCaptureContext(&c);
    LOG_INFO("watch", "stack at %s", tag);
    log_stack(c, 14);
}

void rearm_watch() {
    for (auto& [page, prot] : g_trap_pages) {
        DWORD old;
        VirtualProtect(vm::g_base + (uint64_t(page) << 12), 4096, prot, &old);
    }
    uint64_t now = trace::now_ns();
    std::lock_guard lk(g_trap_lock);
    for (int i = 0; i < static_cast<int>(g_traps.size()); i++) {
        if (g_traps[i].burst_n && now - g_traps[i].burst_t > 50000000) close_burst(i);
    }
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
            DWORD& prot = g_trap_pages[static_cast<uint32_t>(p)];
            prot = (t.reads || prot == PAGE_NOACCESS) ? PAGE_NOACCESS : PAGE_READONLY;
        }
        LOG_INFO("trap", "trap %zu: %s 0x%08x+0x%x%s%s", i, t.reads ? "read/write" : "write", t.addr, t.len, t.has_val ? " value-match" : "", t.dump ? " dump" : "");
    }
    g_watch_count = static_cast<int>(g_traps.size());
    rearm_watch();
}

}
