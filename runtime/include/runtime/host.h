#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace host {

uint64_t qpc();
uint64_t qpf();
uint64_t thread_cycles();
uint64_t thread_cycles(void* native);

void yield();
void sleep_ms(uint32_t ms);
uint32_t tid();
void set_thread_name(const std::string& name);
std::string thread_name();
void* current_native();
bool native_is_current(void* native);
bool set_affinity(const std::vector<int>& lps);
bool set_priority(int prio);
int last_error();

void wait_on(const void* addr, const void* cmp, size_t size, uint32_t ms);
void wake_all(const void* addr);
void wake_one(const void* addr);

enum class Prot { None, Read, ReadWrite };
void* vm_reserve(size_t size);
bool vm_commit(void* p, size_t size);
bool vm_decommit(void* p, size_t size);
bool vm_protect(void* p, size_t size, Prot prot);

struct Core {
    int efficiency = 0;
    std::vector<int> lps;
};
std::vector<Core> cpu_cores();
int logical_cpus();
void init_timers();

}
