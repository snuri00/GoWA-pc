#include "runtime/placement.h"
#include "runtime/host.h"
#include "runtime/image.h"
#include "runtime/loader.h"
#include "runtime/log.h"
#include "runtime/trace.h"
#include "runtime/critpath.h"
#include "runtime/hooks.h"
#include "runtime/memory.h"
#include "gpu/presenter.h"
#include "runtime/thread.h"

#include "runtime/vfs.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

namespace spu {
void status_dump();
void lowfps_tick(uint64_t flips);
}

namespace lv2 {
void rsx_status();
uint64_t rsx_flip_count();
}

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <map>
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace {

uint32_t push_strings(uint32_t& sp, const char* const* list, int count) {
    uint32_t ptrs = 0;
    std::vector<uint32_t> addrs;
    for (int i = 0; i < count; i++) {
        uint32_t len = static_cast<uint32_t>(std::strlen(list[i])) + 1;
        sp -= (len + 15) & ~15u;
        std::memcpy(vm::ptr(sp), list[i], len);
        addrs.push_back(sp);
    }
    sp -= static_cast<uint32_t>((addrs.size() + 1) * 8 + 15) & ~15u;
    ptrs = sp;
    for (size_t i = 0; i < addrs.size(); i++) vm::write64(ptrs + static_cast<uint32_t>(i) * 8, addrs[i]);
    vm::write64(ptrs + static_cast<uint32_t>(addrs.size()) * 8, 0);
    return ptrs;
}

}

namespace crash {
void install();
void arm_watch();
void rearm_watch();
}


namespace {

struct CensusRow {
    double user, kernel;
    std::string name;
    unsigned long tid;
};

void census_log(std::vector<CensusRow>& rows, double total, int period) {
    std::sort(rows.begin(), rows.end(), [](const CensusRow& a, const CensusRow& b) { return a.user + a.kernel > b.user + b.kernel; });
    std::string line;
    char b[160];
    for (const CensusRow& r : rows) {
        std::snprintf(b, sizeof b, " [%s#%lu %.0f%%u %.0f%%k]", r.name.empty() ? "?" : r.name.c_str(), r.tid, r.user, r.kernel);
        line += b;
    }
    LOG_INFO("census", "process cpu %.0f%% of one core over %ds:%s", total, period, line.c_str());
}

#ifdef _WIN32
void thread_census(int period) {
    static std::map<DWORD, std::pair<uint64_t, uint64_t>> prev;
    static auto prev_wall = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    double wall = std::chrono::duration<double>(now - prev_wall).count();
    prev_wall = now;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof te};
    DWORD pid = GetCurrentProcessId();
    using Row = CensusRow;
    std::vector<Row> rows;
    std::map<DWORD, std::pair<uint64_t, uint64_t>> cur;
    double total = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        FILETIME c, e, k, u;
        if (GetThreadTimes(h, &c, &e, &k, &u)) {
            uint64_t ku = (uint64_t(k.dwHighDateTime) << 32) | k.dwLowDateTime, uu = (uint64_t(u.dwHighDateTime) << 32) | u.dwLowDateTime;
            cur[te.th32ThreadID] = {uu, ku};
            auto it = prev.find(te.th32ThreadID);
            if (it != prev.end() && wall > 0) {
                Row r;
                r.user = (uu - it->second.first) / 1e7 / wall * 100.0;
                r.kernel = (ku - it->second.second) / 1e7 / wall * 100.0;
                r.tid = te.th32ThreadID;
                PWSTR desc = nullptr;
                if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc) {
                    char b[128] = {};
                    WideCharToMultiByte(CP_UTF8, 0, desc, -1, b, sizeof b - 1, nullptr, nullptr);
                    r.name = b;
                    LocalFree(desc);
                }
                total += r.user + r.kernel;
                if (r.user + r.kernel >= 1.0) rows.push_back(r);
            }
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    prev.swap(cur);
    census_log(rows, total, period);
}
#else
void thread_census(int period) {
    static std::map<unsigned long, std::pair<uint64_t, uint64_t>> prev;
    static auto prev_wall = std::chrono::steady_clock::now();
    static const double hz = static_cast<double>(sysconf(_SC_CLK_TCK));
    auto now = std::chrono::steady_clock::now();
    double wall = std::chrono::duration<double>(now - prev_wall).count();
    prev_wall = now;
    std::vector<CensusRow> rows;
    std::map<unsigned long, std::pair<uint64_t, uint64_t>> cur;
    double total = 0;
    DIR* d = opendir("/proc/self/task");
    if (!d) return;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        unsigned long tid = std::strtoul(e->d_name, nullptr, 10);
        std::string base = std::string("/proc/self/task/") + e->d_name;
        std::ifstream sf(base + "/stat");
        std::string st;
        std::getline(sf, st);
        size_t rp = st.rfind(')');
        if (rp == std::string::npos) continue;
        unsigned long long uu = 0, ku = 0;
        if (std::sscanf(st.c_str() + rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &uu, &ku) != 2) continue;
        cur[tid] = {uu, ku};
        auto it = prev.find(tid);
        if (it == prev.end() || wall <= 0) continue;
        CensusRow r;
        r.user = (uu - it->second.first) / hz / wall * 100.0;
        r.kernel = (ku - it->second.second) / hz / wall * 100.0;
        r.tid = tid;
        std::ifstream cf(base + "/comm");
        std::getline(cf, r.name);
        total += r.user + r.kernel;
        if (r.user + r.kernel >= 1.0) rows.push_back(r);
    }
    closedir(d);
    prev.swap(cur);
    census_log(rows, total, period);
}
#endif

}

int main(int argc, char** argv) {
    crash::install();
    trace::init();
    critpath::init();
    hooks::init();
    host::init_timers();
    gpu::init();
    if (const char* root = std::getenv("GOWA_ROOT")) loader::g_root = root;
    if (const char* lvl = std::getenv("GOWA_LOG")) {
        if (!std::strcmp(lvl, "trace")) logging::set_level(logging::Level::Trace);
        else if (!std::strcmp(lvl, "debug")) logging::set_level(logging::Level::Debug);
    }
    LOG_INFO("main", "GOWA-pc: %u images, %u functions, %u imports, %u exports, root %s", image::image_count, image::function_count, image::import_count, image::export_count, loader::g_root.c_str());
    if (!vm::init()) return 1;
    int game = loader::find_image("gowa");
    int lv2 = loader::find_image("liblv2");
    if (game < 0 || lv2 < 0 || !loader::load_image(game) || !loader::load_image(lv2)) return 1;
    const loader::ProcessInfo& pi = loader::g_process;
    LOG_INFO("main", "sdk 0x%x prio %d stack 0x%x malloc page 0x%x tls 0x%x/0x%x/0x%x", pi.sdk_version, pi.primary_prio, pi.primary_stacksize, pi.malloc_pagesize, pi.tls_vaddr, pi.tls_filesz, pi.tls_memsz);
    uint32_t stack_size = pi.primary_stacksize;
    switch (stack_size) {
    case 0x10: stack_size = 32 * 1024; break;
    case 0x20: stack_size = 64 * 1024; break;
    case 0x30: stack_size = 96 * 1024; break;
    case 0x40: stack_size = 128 * 1024; break;
    case 0x50: stack_size = 256 * 1024; break;
    case 0x60: stack_size = 512 * 1024; break;
    case 0x70: stack_size = 1024 * 1024; break;
    default: stack_size = std::max<uint32_t>(0x10000, (stack_size + 0xFFF) & ~0xFFFu); break;
    }
    auto main_thread = guest::create("main_thread", image::images[lv2].entry, 0, 0, pi.primary_prio, stack_size, 0, true);
    if (!main_thread) return 1;
    uint32_t sp = main_thread->stack_addr + main_thread->stack_size;
    std::string self_path = vfs::patch_sfo().strings.empty() ? "/dev_bdvd/PS3_GAME/USRDIR/GOWA.SELF" : "/dev_hdd0/game/BCES01741/USRDIR/GOWA.SELF";
    std::vector<std::string> arg_list = {self_path};
    if (const char* extra = std::getenv("GOWA_ARGS")) {
        std::string e = extra;
        size_t pos = 0;
        while (pos < e.size()) {
            size_t end = e.find(' ', pos);
            if (end == std::string::npos) end = e.size();
            if (end > pos) arg_list.push_back(e.substr(pos, end - pos));
            pos = end + 1;
        }
    }
    std::vector<const char*> args;
    for (const std::string& a : arg_list) args.push_back(a.c_str());
    LOG_INFO("main", "argc %zu%s%s", args.size(), args.size() > 1 ? " extra: " : "", std::getenv("GOWA_ARGS") ? std::getenv("GOWA_ARGS") : "");
    uint32_t argv_addr = push_strings(sp, args.data(), static_cast<int>(args.size()));
    uint32_t envp_addr = push_strings(sp, nullptr, 0);
    ppu::Context& c = main_thread->ctx;
    c.gpr[1] = (sp - 0x70) & ~15u;
    c.gpr[3] = args.size();
    c.gpr[4] = argv_addr;
    c.gpr[5] = envp_addr;
    c.gpr[6] = 0;
    c.gpr[7] = main_thread->id;
    c.gpr[8] = pi.tls_vaddr;
    c.gpr[9] = pi.tls_filesz;
    c.gpr[10] = pi.tls_memsz;
    c.gpr[11] = pi.entry_opd;
    c.gpr[12] = pi.malloc_pagesize;
    int watch_delay = std::getenv("GOWA_TRAP_AT") ? std::atoi(std::getenv("GOWA_TRAP_AT")) : std::getenv("GOWA_WATCH_DELAY") ? std::atoi(std::getenv("GOWA_WATCH_DELAY")) : 0;
    if (!watch_delay) crash::arm_watch();
    guest::start(main_thread);
    bool status = std::getenv("GOWA_STATUS") != nullptr;
    int memdump_at = std::getenv("GOWA_MEMDUMP_AT") ? std::atoi(std::getenv("GOWA_MEMDUMP_AT")) : -1;
    host::set_thread_name("Host main");
    placement::apply("host");
    static const int census = std::getenv("GOWA_THREAD_CPU") ? std::atoi(std::getenv("GOWA_THREAD_CPU")) : 0;
    std::vector<uint32_t> peeks;
    if (const char* e = std::getenv("GOWA_PEEK")) {
        for (const char* q = e; *q;) {
            char* end = nullptr;
            peeks.push_back(static_cast<uint32_t>(std::strtoul(q, &end, 16)));
            q = *end ? end + 1 : end;
        }
    }
    for (int tick = 0;; tick++) {
        if (census > 0 && tick % census == 0) thread_census(census);
        hooks::tick_patches();
        spu::lowfps_tick(lv2::rsx_flip_count());
        if (!peeks.empty()) {
            std::string line;
            for (uint32_t addr : peeks) {
                char buf[32];
                std::snprintf(buf, sizeof buf, " %x=%08x", addr, vm::read32(addr));
                line += buf;
            }
            LOG_INFO("peek", "%s", line.c_str());
        }
        if (watch_delay && tick == watch_delay) crash::arm_watch();
        else if (tick > watch_delay) crash::rearm_watch();
        if (tick == memdump_at) trace::dump_memory("memdump.bin");
        if (std::error_code ec; std::filesystem::exists("blackbox.req", ec)) {
            std::filesystem::remove("blackbox.req", ec);
            hooks::capture("external request");
            if (FILE* f = std::fopen("blackbox.done", "w")) std::fclose(f);
        }
        if (status && tick % 3 == 2) {
            spu::status_dump();
            guest::status_dump();
            lv2::rsx_status();
        }
        std::unique_lock lk(main_thread->lock);
        if (main_thread->cv.wait_for(lk, std::chrono::seconds(1), [&] { return main_thread->exited; })) break;
    }
    LOG_INFO("main", "main thread finished");
    trace::shutdown();
    return 0;
}
