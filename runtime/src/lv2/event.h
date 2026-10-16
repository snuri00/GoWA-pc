#pragma once

#include "object.h"

#include <deque>

namespace lv2 {

struct Event {
    uint64_t source;
    uint64_t data1;
    uint64_t data2;
    uint64_t data3;
};

struct EventQueue : Object {
    std::mutex m;
    std::condition_variable cv;
    std::deque<Event> events;
    uint32_t size = 127;
    int32_t type = 1;
    uint64_t key = 0;
    bool destroyed = false;
    uint32_t waiters = 0;
};

int64_t push_event(const std::shared_ptr<EventQueue>& q, const Event& e);
std::shared_ptr<EventQueue> queue_by_key(uint64_t key);

}
