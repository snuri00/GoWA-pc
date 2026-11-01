#include "runtime/hostsleep.h"
#include "object.h"
#include "runtime/loader.h"
#include "runtime/vfs.h"
#include "runtime/log.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace lv2 {

LV2_SYSCALL(1, sys_process_getpid) {
    return 1;
}

LV2_SYSCALL(14, sys_process_is_spu_lock_line_reservation_address) {
    return CELL_OK;
}

LV2_SYSCALL(22, _sys_process_exit) {
    LOG_INFO("process", "_sys_process_exit(0x%x)", u32arg(ctx, 0));
    throw ProcessExit{s32arg(ctx, 0)};
}

LV2_SYSCALL(25, sys_process_get_sdk_version) {
    uint32_t out = u32arg(ctx, 1);
    if (!out) return CELL_EFAULT;
    vm::write32(out, loader::g_process.sdk_version);
    return CELL_OK;
}

LV2_SYSCALL(30, _sys_process_get_paramsfo) {
    uint32_t buf = u32arg(ctx, 0);
    std::string id = vfs::disc_sfo().str("TITLE_ID");
    if (id.empty()) return CELL_ENOENT;
    std::memset(vm::ptr(buf), 0, 0x40);
    std::memcpy(vm::ptr(buf + 1), id.data(), id.size() < 9 ? id.size() : 9);
    return CELL_OK;
}

LV2_SYSCALL(988, sys_dbg_register_ppu_exception_handler) {
    return CELL_OK;
}

LV2_SYSCALL(402, sys_tty_read) {
    return CELL_EIO;
}

LV2_SYSCALL(403, sys_tty_write) {
    uint32_t ch = u32arg(ctx, 0);
    uint32_t buf = u32arg(ctx, 1);
    uint32_t len = u32arg(ctx, 2);
    uint32_t pw = u32arg(ctx, 3);
    std::string s(reinterpret_cast<const char*>(vm::ptr(buf)), len);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    LOG_INFO("tty", "[%u] %s", ch, s.c_str());
    if (pw) vm::write32(pw, len);
    return CELL_OK;
}

LV2_SYSCALL(147, sys_time_get_timebase_frequency) {
    return 80000000;
}

LV2_SYSCALL(146, sys_time_set_current_time) {
    return CELL_ENOSYS;
}

LV2_SYSCALL(149, sys_time_get_system_time) {
    return 0;
}

LV2_SYSCALL(145, sys_time_get_current_time) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = (uint64_t(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) - 116444736000000000ull;
#else
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t t = uint64_t(ts.tv_sec) * 10000000ull + uint64_t(ts.tv_nsec) / 100;
#endif
    if (uint32_t s = u32arg(ctx, 0)) vm::write64(s, t / 10000000ull);
    if (uint32_t n = u32arg(ctx, 1)) vm::write64(n, (t % 10000000ull) * 100);
    return CELL_OK;
}

LV2_SYSCALL(144, sys_time_get_timezone) {
#ifdef _WIN32
    TIME_ZONE_INFORMATION tz;
    DWORD r = GetTimeZoneInformation(&tz);
    int32_t bias = -tz.Bias, dst = r == TIME_ZONE_ID_DAYLIGHT ? -tz.DaylightBias : 0;
#else
    std::time_t now = std::time(nullptr);
    std::tm lt{};
    localtime_r(&now, &lt);
    int32_t dst = lt.tm_isdst > 0 ? 60 : 0;
    int32_t bias = static_cast<int32_t>(lt.tm_gmtoff / 60) - dst;
#endif
    if (uint32_t p = u32arg(ctx, 0)) vm::write32(p, static_cast<uint32_t>(bias));
    if (uint32_t p = u32arg(ctx, 1)) vm::write32(p, static_cast<uint32_t>(dst));
    return CELL_OK;
}

LV2_SYSCALL(141, sys_timer_usleep) {
    uint64_t us = u64arg(ctx, 0);
    static const long min_us = std::getenv("GOWA_USLEEP_MIN") ? std::atol(std::getenv("GOWA_USLEEP_MIN")) : 100;
    static std::atomic<uint64_t> hist[8];
    static const bool stats = std::getenv("GOWA_USLEEP_STATS") != nullptr;
    if (stats) {
        int b = us < 10 ? 0 : us < 30 ? 1 : us < 100 ? 2 : us < 300 ? 3 : us < 1000 ? 4 : us < 5000 ? 5 : 6;
        if (hist[b].fetch_add(1) % 100000 == 99999 || (hist[7].fetch_add(1) & 0x3FFFF) == 0)
            LOG_INFO("lv2", "usleep histogram <10:%llu <30:%llu <100:%llu <300:%llu <1000:%llu <5000:%llu more:%llu", (unsigned long long)hist[0].load(), (unsigned long long)hist[1].load(), (unsigned long long)hist[2].load(),
                     (unsigned long long)hist[3].load(), (unsigned long long)hist[4].load(), (unsigned long long)hist[5].load(), (unsigned long long)hist[6].load());
    }
    if (min_us && us < uint64_t(min_us)) us = uint64_t(min_us);
    host::sleep_us(static_cast<uint64_t>(us / ppu::time_scale()));
    return CELL_OK;
}

LV2_SYSCALL(142, sys_timer_sleep) {
    std::this_thread::sleep_for(std::chrono::seconds(u32arg(ctx, 0)));
    return CELL_OK;
}

LV2_SYSCALL(865, sys_ss_random_number_generator) {
    uint32_t buf = u32arg(ctx, 1);
    uint64_t size = u64arg(ctx, 2);
    for (uint64_t i = 0; i < size; i++) vm::ptr(buf)[i] = static_cast<uint8_t>(std::rand());
    return CELL_OK;
}

LV2_SYSCALL(871, sys_ss_access_control_engine) {
    uint64_t pkg = u64arg(ctx, 0);
    if (pkg == 2) {
        vm::write64(u32arg(ctx, 1), 0x1010000001000003ull);
        return CELL_OK;
    }
    return CELL_ENOSYS;
}

LV2_SYSCALL(872, sys_ss_get_open_psid) {
    if (uint32_t p = u32arg(ctx, 0)) {
        vm::write64(p, 0x0123456789ABCDEFull);
        vm::write64(p + 8, 0xFEDCBA9876543210ull);
    }
    return CELL_OK;
}

}
