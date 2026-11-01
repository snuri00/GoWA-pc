#include "runtime/host.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#else
#include <cerrno>
#include <climits>
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <map>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <time.h>
#include <unistd.h>
#include <x86intrin.h>
#endif

namespace host {

#ifdef _WIN32

uint64_t qpc() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart);
}

uint64_t qpf() {
    static const uint64_t f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return static_cast<uint64_t>(v.QuadPart);
    }();
    return f;
}

uint64_t thread_cycles() {
    ULONG64 c = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c);
    return c;
}

uint64_t thread_cycles(void* native) {
    ULONG64 c = 0;
    if (native) QueryThreadCycleTime(static_cast<HANDLE>(native), &c);
    return c;
}

void yield() { SwitchToThread(); }
void sleep_ms(uint32_t ms) { Sleep(ms); }
uint32_t tid() { return GetCurrentThreadId(); }

void set_thread_name(const std::string& name) {
    wchar_t w[128];
    if (MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, w, 128) > 0) SetThreadDescription(GetCurrentThread(), w);
}

std::string thread_name() {
    PWSTR w = nullptr;
    std::string name;
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

void* current_native() {
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    return h;
}

bool native_is_current(void* native) { return GetThreadId(static_cast<HANDLE>(native)) == GetCurrentThreadId(); }

bool set_affinity(const std::vector<int>& lps) {
    DWORD_PTR m = 0;
    for (int lp : lps) {
        if (lp >= 0 && lp < 64) m |= DWORD_PTR(1) << lp;
    }
    return m && SetThreadAffinityMask(GetCurrentThread(), m) != 0;
}

bool set_priority(int prio) { return SetThreadPriority(GetCurrentThread(), prio) != 0; }
int last_error() { return static_cast<int>(GetLastError()); }

void wait_on(const void* addr, const void* cmp, size_t size, uint32_t ms) { WaitOnAddress(const_cast<void*>(addr), const_cast<void*>(cmp), size, ms); }
void wake_all(const void* addr) { WakeByAddressAll(const_cast<void*>(addr)); }
void wake_one(const void* addr) { WakeByAddressSingle(const_cast<void*>(addr)); }

void* vm_reserve(size_t size) { return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS); }
bool vm_commit(void* p, size_t size) { return VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE) != nullptr; }
bool vm_decommit(void* p, size_t size) { return VirtualFree(p, size, MEM_DECOMMIT) != 0; }

bool vm_protect(void* p, size_t size, Prot prot) {
    DWORD old;
    DWORD np = prot == Prot::ReadWrite ? PAGE_READWRITE : prot == Prot::Read ? PAGE_READONLY : PAGE_NOACCESS;
    return VirtualProtect(p, size, np, &old) != 0;
}

std::vector<Core> cpu_cores() {
    std::vector<Core> cores;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    std::vector<uint8_t> buf(len);
    if (!len || !GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data()), &len)) return cores;
    for (DWORD off = 0; off < len;) {
        auto* e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        if (e->Relationship == RelationProcessorCore && e->Processor.GroupCount >= 1 && e->Processor.GroupMask[0].Group == 0) {
            Core c;
            c.efficiency = e->Processor.EfficiencyClass;
            KAFFINITY m = e->Processor.GroupMask[0].Mask;
            for (int i = 0; i < 64; i++) {
                if (m & (KAFFINITY(1) << i)) c.lps.push_back(i);
            }
            if (!c.lps.empty()) cores.push_back(c);
        }
        off += e->Size;
    }
    return cores;
}

int logical_cpus() {
    DWORD_PTR proc = 0, sys = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys)) return 0;
    int n = 0;
    for (DWORD_PTR m = proc; m; m &= m - 1) n++;
    return n;
}

void init_timers() { timeBeginPeriod(1); }

#else

namespace {

uint64_t clock_ns(clockid_t id) {
    timespec ts;
    clock_gettime(id, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

double tsc_per_ns() {
    static const double r = [] {
        uint64_t n0 = clock_ns(CLOCK_MONOTONIC), t0 = __rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        uint64_t n1 = clock_ns(CLOCK_MONOTONIC), t1 = __rdtsc();
        return double(t1 - t0) / double(n1 - n0);
    }();
    return r;
}

std::string read_line(const std::string& path) {
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    return s;
}

std::vector<int> parse_list(const std::string& s) {
    std::vector<int> out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        std::string item = s.substr(pos, end - pos);
        size_t dash = item.find('-');
        if (!item.empty()) {
            int a = std::atoi(item.c_str()), b = dash == std::string::npos ? a : std::atoi(item.c_str() + dash + 1);
            for (int i = a; i <= b; i++) out.push_back(i);
        }
        pos = end + 1;
    }
    return out;
}

}

uint64_t qpc() { return clock_ns(CLOCK_MONOTONIC); }
uint64_t qpf() { return 1000000000ull; }

uint64_t thread_cycles() { return static_cast<uint64_t>(double(clock_ns(CLOCK_THREAD_CPUTIME_ID)) * tsc_per_ns()); }

uint64_t thread_cycles(void* native) {
    if (!native) return 0;
    clockid_t id;
    if (pthread_getcpuclockid(reinterpret_cast<pthread_t>(native), &id) != 0) return 0;
    return static_cast<uint64_t>(double(clock_ns(id)) * tsc_per_ns());
}

void yield() { sched_yield(); }

void sleep_ms(uint32_t ms) {
    if (!ms) {
        sched_yield();
        return;
    }
    timespec ts{static_cast<time_t>(ms / 1000), static_cast<long>(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

uint32_t tid() {
    thread_local const uint32_t t = static_cast<uint32_t>(syscall(SYS_gettid));
    return t;
}

void set_thread_name(const std::string& name) { pthread_setname_np(pthread_self(), name.substr(0, 15).c_str()); }

std::string thread_name() {
    char b[32] = {};
    pthread_getname_np(pthread_self(), b, sizeof b);
    return b;
}

void* current_native() { return reinterpret_cast<void*>(pthread_self()); }
bool native_is_current(void* native) { return pthread_equal(reinterpret_cast<pthread_t>(native), pthread_self()) != 0; }

bool set_affinity(const std::vector<int>& lps) {
    cpu_set_t set;
    CPU_ZERO(&set);
    bool any = false;
    for (int lp : lps) {
        if (lp >= 0 && lp < CPU_SETSIZE) {
            CPU_SET(lp, &set);
            any = true;
        }
    }
    return any && pthread_setaffinity_np(pthread_self(), sizeof set, &set) == 0;
}

bool set_priority(int prio) {
    int nice = std::clamp(-prio * 2, -20, 19);
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(tid()), nice) == 0) return true;
    return nice < 0 && (errno == EACCES || errno == EPERM);
}

int last_error() { return errno; }

void wait_on(const void* addr, const void* cmp, size_t size, uint32_t ms) {
    uint32_t expect;
    std::memcpy(&expect, cmp, 4);
    (void)size;
    timespec ts{static_cast<time_t>(ms / 1000), static_cast<long>(ms % 1000) * 1000000L};
    syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expect, ms == UINT32_MAX ? nullptr : &ts, nullptr, 0);
}

void wake_all(const void* addr) { syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0); }
void wake_one(const void* addr) { syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0); }

void* vm_reserve(size_t size) {
    void* p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

bool vm_commit(void* p, size_t size) { return mprotect(p, size, PROT_READ | PROT_WRITE) == 0; }

bool vm_decommit(void* p, size_t size) {
    madvise(p, size, MADV_DONTNEED);
    return mprotect(p, size, PROT_NONE) == 0;
}

bool vm_protect(void* p, size_t size, Prot prot) {
    int np = prot == Prot::ReadWrite ? PROT_READ | PROT_WRITE : prot == Prot::Read ? PROT_READ : PROT_NONE;
    return mprotect(p, size, np) == 0;
}

std::vector<Core> cpu_cores() {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    sched_getaffinity(0, sizeof allowed, &allowed);
    std::vector<int> atoms = parse_list(read_line("/sys/devices/cpu_atom/cpus"));
    std::map<std::string, Core> by_core;
    std::vector<std::string> order;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, &allowed)) continue;
        std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
        std::string key = read_line(base + "core_cpus_list");
        if (key.empty()) key = read_line(base + "thread_siblings_list");
        if (key.empty()) key = std::to_string(cpu);
        auto [it, fresh] = by_core.try_emplace(key);
        if (fresh) {
            order.push_back(key);
            it->second.efficiency = std::find(atoms.begin(), atoms.end(), cpu) == atoms.end() ? 1 : 0;
        }
        it->second.lps.push_back(cpu);
    }
    std::vector<Core> cores;
    for (auto& k : order) cores.push_back(by_core[k]);
    return cores;
}

int logical_cpus() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) != 0) return 0;
    return CPU_COUNT(&set);
}

void init_timers() {}

#endif

}
