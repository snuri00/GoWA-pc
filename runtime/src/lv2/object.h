#pragma once

#include "runtime/kernel.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/thread.h"

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace lv2 {

struct Object {
    virtual ~Object() = default;
    std::string name;
};

uint32_t add_object(uint32_t base, std::shared_ptr<Object> obj);
std::shared_ptr<Object> get_object(uint32_t id);
bool remove_object(uint32_t id);

template <class T>
std::shared_ptr<T> get(uint32_t id) {
    return std::dynamic_pointer_cast<T>(get_object(id));
}

inline uint32_t u32arg(ppu::Context& c, int n) { return static_cast<uint32_t>(c.gpr[3 + n]); }
inline uint64_t u64arg(ppu::Context& c, int n) { return c.gpr[3 + n]; }
inline int32_t s32arg(ppu::Context& c, int n) { return static_cast<int32_t>(c.gpr[3 + n]); }

inline uint32_t current_thread_id() {
    guest::Thread* t = guest::current();
    return t ? t->id : 0;
}

inline std::string name8(uint32_t addr) {
    if (!addr) return {};
    char b[9] = {};
    std::memcpy(b, vm::ptr(addr), 8);
    return b;
}

inline std::string name_u64(uint64_t v) {
    char b[9] = {};
    for (int i = 0; i < 8; i++) b[i] = static_cast<char>(v >> (56 - 8 * i));
    return b;
}

template <class Pred>
bool wait_for(std::unique_lock<std::mutex>& lk, std::condition_variable& cv, uint64_t timeout_us, Pred pred) {
    if (!timeout_us) {
        cv.wait(lk, pred);
        return true;
    }
    return cv.wait_for(lk, std::chrono::microseconds(timeout_us), pred);
}

uint64_t system_time_us();

}
