#pragma once

#include "runtime/jobsys.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace spu {
struct SpuThread;
}

namespace jobsys {

constexpr bool kDefaultOn = true;

struct Config {
    bool on = false;
    bool all_mods = true;
    std::vector<uint32_t> mods;
    std::vector<uint32_t> add_mods;
    std::vector<uint32_t> del_mods;
    Tier tier = Tier::Auto;
    std::vector<std::pair<uint32_t, Tier>> tier_mod;
    uint32_t verify = 0;
    int workers = 1;
    std::vector<int> cpus;
    int prio = 1;
    bool help = true;
    bool help_sites = true;
    bool donate = false;
    int donor_spu = -1;
    std::vector<uint32_t> sandbox;
    std::vector<uint32_t> vpoison;
    uint32_t poison = 0;
    bool stats = true;
    bool ordered = true;
    int relax_sleep_us = 40;
    bool fast_mfc = true;
};

const Config& cfg();

void update_line(uint32_t ea, bool (*fn)(uint8_t* line, uint32_t off, void* user), void* user);
void kick_workload(uint32_t spurs, uint32_t wid, uint8_t value);
void dep_decrement(uint32_t ea, uint32_t spurs);
void relax_spin(uint32_t& n);
void set_relax_hard(bool on);
void for_each_module(void (*fn)(const ModuleInfo&, void*), void* user);

struct Recorder;

struct Executor {
    std::unique_ptr<spu::SpuThread> t;
    std::unique_ptr<spu::SpuThread> shadow;
    std::unique_ptr<Recorder> rec;
    uint32_t index = 0;
    std::unordered_map<uint32_t, std::vector<uint8_t>> slots;
    std::vector<uint8_t> arena;
    size_t arena_used = 0;
    Tier last = Tier::Wws;
    std::vector<uint8_t> start_ls;
    alignas(16) uint8_t start_gpr[128 * 16] = {};
    uint32_t last_module = 0;
};

Executor& thread_executor(uint32_t index);
void prepare_job(Executor& x, const JobDesc& j);
bool exec_job(Executor& x, const JobDesc& j, Tier tier);

struct Stats {
    std::atomic<uint64_t> jobs{0}, helped{0}, donated{0}, wsites{0}, wsite_jobs{0};
    std::atomic<uint64_t> by_tier[4]{};
    std::atomic<uint64_t> tier_cycles[4]{};
    std::atomic<uint64_t> resumed{0}, slow_dma{0}, fallback{0}, captured{0}, gated{0}, ordered_waits{0};
    std::atomic<uint64_t> wait_n{0}, wait_cyc{0}, park_n{0};
    std::atomic<uint64_t> max_job_cyc{0}, max_wait_cyc{0};
    std::atomic<uint64_t> decl[8]{};
    std::atomic<uint64_t> relax_sleeps{0};
};
Stats& stats();
double tsc_hz();
void start_stats();

struct ModStat {
    uint32_t ea = 0;
    std::atomic<uint64_t> jobs{0}, cyc{0}, fallback{0}, max_cyc{0};
};
ModStat& mod_stat(uint32_t ea);
void help_until_list(const Completion& c, uint32_t list);
uint32_t queued();
void start_workers();

}
