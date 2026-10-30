#include "runtime/log.h"
#include "runtime/host.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace logging {

namespace {

std::mutex g_lock;
std::atomic<int> g_level{static_cast<int>(Level::Info)};
FILE* g_file = nullptr;
const auto g_start = std::chrono::steady_clock::now();

const char* tag(Level l) {
    switch (l) {
    case Level::Trace: return "T";
    case Level::Debug: return "D";
    case Level::Info: return "I";
    case Level::Warning: return "W";
    case Level::Error: return "E";
    default: return "F";
    }
}

}

void set_level(Level level) { g_level = static_cast<int>(level); }

bool enabled(Level level) { return static_cast<int>(level) >= g_level.load(std::memory_order_relaxed); }

void write(Level level, const char* channel, const char* fmt, ...) {
    if (!enabled(level)) return;
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    std::lock_guard lk(g_lock);
    if (!g_file) g_file = std::fopen("gowa.log", "w");
    char line[2300];
    int n = std::snprintf(line, sizeof line, "%s %10.6f [%5u] %s: %s\n", tag(level), t, host::tid(), channel, msg);
    if (g_file) {
        std::fwrite(line, 1, n, g_file);
        if (level >= Level::Info) std::fflush(g_file);
    }
    if (level >= Level::Info) std::fwrite(line, 1, n, stderr);
    if (level == Level::Fatal) {
        if (g_file) std::fflush(g_file);
        std::abort();
    }
}

double uptime() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count(); }

unsigned long long uptime_ns() { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_start).count()); }

}
