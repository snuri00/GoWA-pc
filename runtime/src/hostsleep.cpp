#include "runtime/hostsleep.h"

#include <chrono>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <time.h>
#endif

namespace host {

namespace {

#ifdef _WIN32
HANDLE timer() {
    thread_local HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    return t;
}
#endif

}

void sleep_us(uint64_t us) {
    if (us == 0) {
        std::this_thread::yield();
        return;
    }
    if (us < 100) {
        auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
        while (std::chrono::steady_clock::now() < end) std::this_thread::yield();
        return;
    }
#ifdef _WIN32
    HANDLE t = timer();
    if (!t) {
        std::this_thread::sleep_for(std::chrono::microseconds(us));
        return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(us * 10);
    SetWaitableTimerEx(t, &due, 0, nullptr, nullptr, nullptr, 0);
    WaitForSingleObject(t, INFINITE);
#else
    timespec ts{static_cast<time_t>(us / 1000000), static_cast<long>(us % 1000000) * 1000L};
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, &ts) == EINTR) {
    }
#endif
}

void sleep_until_ns(int64_t steady_ns) {
    int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (steady_ns > now) sleep_us(static_cast<uint64_t>((steady_ns - now) / 1000));
}

}
