#include "runtime/reservation.h"
#include "runtime/memory.h"

#include "runtime/log.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <cstring>
#include <mutex>
#include <thread>

namespace rsv {

namespace {

struct alignas(64) Entry {
    std::atomic<uint64_t> ver{0};
    std::mutex lock;
};

Entry g_table[512];

Entry& entry(uint32_t addr) { return g_table[(addr & 0xFF80) >> 7]; }

}

std::atomic<uint64_t>& version(uint32_t addr) { return entry(addr).ver; }

uint64_t snapshot(uint32_t line, uint8_t* out) {
    Entry& e = entry(line);
    for (;;) {
        uint64_t v1 = e.ver.load(std::memory_order_acquire);
        std::memcpy(out, vm::ptr(line), 128);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (e.ver.load(std::memory_order_acquire) == v1) return v1;
    }
}

bool watched_line(uint32_t line) {
    static const std::vector<uint32_t> lines = [] {
        std::vector<uint32_t> v;
        if (const char* e = std::getenv("GOWA_RSV_LOG")) {
            std::string s = e;
            size_t pos = 0;
            while (pos < s.size()) {
                size_t end = s.find(',', pos);
                if (end == std::string::npos) end = s.size();
                v.push_back(static_cast<uint32_t>(std::strtoul(s.substr(pos, end - pos).c_str(), nullptr, 16)) & ~127u);
                pos = end + 1;
            }
        }
        return v;
    }();
    for (uint32_t l : lines) if (l == line) return true;
    return false;
}

void log_change(uint32_t line, uint32_t offset, const uint8_t* data, uint32_t size, const char* how) {
    if (!watched_line(line)) return;
    const uint8_t* cur = vm::ptr(line);
    char buf[512];
    int n = 0;
    for (uint32_t i = 0; i < size && n < 480; i++) {
        uint32_t o = offset + i;
        if (cur[o] != data[i]) n += std::snprintf(buf + n, sizeof buf - n, " [%02x]%02x->%02x", o, cur[o], data[i]);
    }
    if (n) LOG_INFO("rsv", "%s line 0x%08x:%s", how, line, buf);
}

bool commit_if(uint32_t line, uint64_t rtime, const uint8_t* expected, const uint8_t* data, uint32_t offset, uint32_t size) {
    Entry& e = entry(line);
    std::lock_guard lk(e.lock);
    if (e.ver.load(std::memory_order_acquire) != rtime) return false;
    if (std::memcmp(vm::ptr(line + offset), expected, size) != 0) return false;
    log_change(line, offset, data, size, size == 128 ? "putllc" : "stcx");
    std::memcpy(vm::ptr(line + offset), data, size);
    e.ver.fetch_add(128, std::memory_order_release);
    e.ver.notify_all();
    return true;
}

void store_unconditional(uint32_t line, const uint8_t* data) {
    Entry& e = entry(line);
    std::lock_guard lk(e.lock);
    log_change(line, 0, data, 128, "putlluc");
    std::memcpy(vm::ptr(line), data, 128);
    e.ver.fetch_add(128, std::memory_order_release);
    e.ver.notify_all();
}

void store_locked(uint32_t addr, const void* data, uint32_t size) {
    Entry& e = entry(addr & ~127u);
    std::lock_guard lk(e.lock);
    log_change(addr & ~127u, addr & 127u, static_cast<const uint8_t*>(data), size, "store");
    std::memcpy(vm::ptr(addr), data, size);
    e.ver.fetch_add(128, std::memory_order_release);
    e.ver.notify_all();
}

bool lost(uint32_t line, uint64_t rtime, const uint8_t* rdata) {
    if (entry(line).ver.load(std::memory_order_acquire) != rtime) return true;
    return std::memcmp(vm::ptr(line), rdata, 128) != 0;
}

void wait_change(uint32_t line, uint64_t rtime, uint32_t micros) {
    Entry& e = entry(line);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(micros);
    while (e.ver.load(std::memory_order_acquire) == rtime && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
}

}
