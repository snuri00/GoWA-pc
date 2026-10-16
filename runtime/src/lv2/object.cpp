#include "object.h"

namespace lv2 {

namespace {

std::mutex g_lock;
std::map<uint32_t, std::shared_ptr<Object>> g_objects;
std::map<uint32_t, uint32_t> g_next;

}

uint32_t add_object(uint32_t base, std::shared_ptr<Object> obj) {
    std::lock_guard lk(g_lock);
    uint32_t& next = g_next[base];
    uint32_t id = base | (++next * 0x100 + 0x01);
    while (g_objects.count(id)) id = base | (++next * 0x100 + 0x01);
    g_objects[id] = std::move(obj);
    return id;
}

std::shared_ptr<Object> get_object(uint32_t id) {
    std::lock_guard lk(g_lock);
    auto it = g_objects.find(id);
    return it == g_objects.end() ? nullptr : it->second;
}

bool remove_object(uint32_t id) {
    std::lock_guard lk(g_lock);
    return g_objects.erase(id) != 0;
}

uint64_t system_time_us() {
    static const auto start = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()) + 1000000;
}

}
