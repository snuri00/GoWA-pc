#pragma once

#include "object.h"

#include <list>

namespace lv2 {

struct LwWaiter {
    uint32_t tid;
    bool woken = false;
    int64_t result = CELL_OK;
};

struct LwMutex : Object {
    std::mutex m;
    std::condition_variable cv;
    int signaled = 0;
    std::list<LwWaiter*> sleepers;
    uint32_t control = 0;
    uint32_t protocol = 0;
    int32_t lwcond_waiters = 0;

    int64_t sleep(std::unique_lock<std::mutex>& lk, LwWaiter& w, uint64_t timeout);
    void release(int64_t result);
};

}
