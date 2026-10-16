#include "runtime/reservation.h"
#include "runtime/memory.h"

#include <chrono>
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

bool commit_if(uint32_t line, uint64_t rtime, const uint8_t* expected, const uint8_t* data, uint32_t offset, uint32_t size) {
    Entry& e = entry(line);
    std::lock_guard lk(e.lock);
    if (e.ver.load(std::memory_order_acquire) != rtime) return false;
    if (std::memcmp(vm::ptr(line + offset), expected, size) != 0) return false;
    std::memcpy(vm::ptr(line + offset), data, size);
    e.ver.fetch_add(128, std::memory_order_release);
    e.ver.notify_all();
    return true;
}

void store_unconditional(uint32_t line, const uint8_t* data) {
    Entry& e = entry(line);
    std::lock_guard lk(e.lock);
    std::memcpy(vm::ptr(line), data, 128);
    e.ver.fetch_add(128, std::memory_order_release);
    e.ver.notify_all();
}

void store_locked(uint32_t addr, const void* data, uint32_t size) {
    Entry& e = entry(addr & ~127u);
    std::lock_guard lk(e.lock);
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
