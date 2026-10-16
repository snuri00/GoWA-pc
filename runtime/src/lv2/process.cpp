#include "object.h"
#include "runtime/loader.h"

#include <cstdio>
#include <ctime>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace lv2 {

LV2_SYSCALL(1, sys_process_getpid) {
    return 0x01000500;
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
    return CELL_ENOENT;
}

LV2_SYSCALL(988, sys_dbg_register_ppu_exception_handler) {
    return CELL_OK;
}

LV2_SYSCALL(402, sys_tty_read) {
    if (uint32_t p = u32arg(ctx, 3)) vm::write32(p, 0);
    return CELL_OK;
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
    return 79800000;
}

LV2_SYSCALL(146, sys_time_get_system_time) {
    return static_cast<int64_t>(system_time_us());
}

LV2_SYSCALL(145, sys_time_get_current_time) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = (uint64_t(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) - 116444736000000000ull;
    if (uint32_t s = u32arg(ctx, 0)) vm::write64(s, t / 10000000ull);
    if (uint32_t n = u32arg(ctx, 1)) vm::write64(n, (t % 10000000ull) * 100);
    return CELL_OK;
}

LV2_SYSCALL(144, sys_time_get_timezone) {
    TIME_ZONE_INFORMATION tz;
    DWORD r = GetTimeZoneInformation(&tz);
    if (uint32_t p = u32arg(ctx, 0)) vm::write32(p, static_cast<uint32_t>(-tz.Bias));
    if (uint32_t p = u32arg(ctx, 1)) vm::write32(p, r == TIME_ZONE_ID_DAYLIGHT ? static_cast<uint32_t>(-tz.DaylightBias) : 0);
    return CELL_OK;
}

LV2_SYSCALL(141, sys_timer_usleep) {
    uint64_t us = u64arg(ctx, 0);
    if (us) std::this_thread::sleep_for(std::chrono::microseconds(us));
    else std::this_thread::yield();
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

LV2_SYSCALL(872, sys_ss_get_open_psid) {
    if (uint32_t p = u32arg(ctx, 0)) {
        vm::write64(p, 0x0123456789ABCDEFull);
        vm::write64(p + 8, 0xFEDCBA9876543210ull);
    }
    return CELL_OK;
}

}
