#pragma once

#include "ppu/context.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace guest {

struct Thread {
    uint32_t id = 0;
    std::string name;
    ppu::Context ctx{};
    uint32_t entry_opd = 0;
    uint32_t stack_addr = 0;
    uint32_t stack_size = 0;
    int32_t prio = 0;
    bool joinable = true;
    bool started = false;
    bool exited = false;
    bool joined = false;
    uint64_t exit_value = 0;
    std::mutex lock;
    std::condition_variable cv;
    std::thread host;
    std::atomic<uint32_t> in_syscall{0};
    std::atomic<uint64_t> syscalls{0};
    uint64_t syscalls_seen = 0;
    uint32_t syscall_hist[1024] = {};
    void* native = nullptr;
};

Thread* current();
std::shared_ptr<Thread> find(uint32_t id);
std::shared_ptr<Thread> create(const std::string& name, uint32_t entry_opd, uint64_t arg0, uint64_t arg1, int32_t prio, uint32_t stack_size, uint32_t tls, bool joinable);
void start(const std::shared_ptr<Thread>& t);
std::shared_ptr<Thread> spawn_host(const std::string& name, int32_t prio, uint32_t stack_size, uint32_t tls, std::function<void(Thread&)> fn);
void join_host(const std::shared_ptr<Thread>& t);
void remove(uint32_t id);
void status_dump();
void call_opd(ppu::Context& ctx, uint8_t* base, uint32_t opd);

}
