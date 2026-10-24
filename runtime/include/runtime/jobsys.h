#pragma once

#include "spu/context.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ppu {
struct Context;
}

namespace renjob {
struct ChanHost;
}

namespace jobsys {

constexpr uint32_t kMaxLoads = 16;
constexpr uint32_t kMaxSets = 16;
constexpr uint32_t kMaxDeps = 4;
constexpr uint32_t kMaxCmd = 0x100;
constexpr uint32_t kPmEa = 0x7a7b00;
constexpr uint32_t kJobReturn = 0x3368;
constexpr uint32_t kWaiterIndex = 31;

enum class Tier : uint8_t { Semantic = 0, Whole = 1, Executor = 2, Wws = 3, Auto = 0xff };

const char* tier_name(Tier t);

struct BufferSet {
    uint8_t first_page = 0;
    uint8_t pages = 0;
    uint8_t buffers = 0;
    bool reserved = false;
    uint32_t ls(uint32_t buf) const { return (uint32_t(first_page) + buf * uint32_t(pages)) * 0x400u; }
};

struct Load {
    uint8_t set = 0;
    uint8_t buf = 0;
    bool list = false;
    bool code = false;
    uint32_t ea = 0;
    uint32_t size = 0;
    uint32_t word = 0;
};

struct JobDesc {
    uint64_t header = 0;
    uint32_t cmd_ea = 0;
    uint32_t cmd_size = 0;
    uint32_t list_obj = 0;
    uint32_t list_hdr = 0;
    uint32_t spurs = 0;
    uint32_t wid = 0;
    uint32_t module_ea = 0;
    uint32_t module_size = 0;
    uint32_t entry = 0;
    BufferSet sets[kMaxSets];
    Load loads[kMaxLoads];
    uint32_t nloads = 0;
    uint32_t deps[kMaxDeps] = {};
    uint32_t ndeps = 0;
    uint32_t commit_mask = 0;
    uint32_t param_off = 0;
    uint32_t gate_ea = 0;
    uint32_t submit_lr = 0;
    uint64_t seq = 0;
    uint64_t t_submit = 0;
    alignas(16) uint8_t cmd[kMaxCmd] = {};
    uint32_t param_ea() const { return cmd_ea + param_off; }
    uint32_t param_size() const { return cmd_size > param_off ? cmd_size - param_off : 0; }
    const uint8_t* params() const { return cmd + param_off; }
    uint32_t param32(uint32_t off) const {
        return off + 4 <= param_size() ? (uint32_t(params()[off]) << 24 | uint32_t(params()[off + 1]) << 16 | uint32_t(params()[off + 2]) << 8 | params()[off + 3]) : 0;
    }
    bool only_code() const { return nloads == 1 && loads[0].code; }
};

enum class Decode : uint8_t { Ok = 0, BadHeader, BadBlock, BadOp, BadLoad, NoRun, NoCode, TooMany };
Decode decode(uint64_t header, uint32_t list_obj, JobDesc& out);
const char* decode_name(Decode d);

struct Mem {
    virtual void get(void* dst, uint32_t ea, uint32_t size) = 0;
    virtual void put(uint32_t ea, const void* src, uint32_t size) = 0;
    virtual uint64_t getllar(uint32_t ea, void* line) = 0;
    virtual bool putllc(uint32_t ea, uint64_t rtime, const void* expect, const void* line) = 0;
    virtual void putlluc(uint32_t ea, const void* line) = 0;
    virtual uint32_t read32(uint32_t ea) = 0;

protected:
    ~Mem() = default;
};

Mem& live_mem();

struct Executor;

struct JobCtx {
    uint32_t worker = 0;
    const JobDesc* job = nullptr;
    Mem* mem = nullptr;
    Executor* exec = nullptr;
    uint8_t* ls = nullptr;
    spu::Context* regs = nullptr;
    uint32_t spins = 0;
    void* slot(uint32_t key, size_t bytes);
    uint8_t* scratch(size_t bytes);
    void dep_decrement(uint32_t ea);
    void relax();
    void progress() { spins = 0; }
};

using Body = bool (*)(const JobDesc& job, JobCtx& ctx);

struct WholeResult {
    bool done = false;
    uint32_t fail_pc = 0;
    uint32_t icall_miss = 0;
};

using WholeFn = WholeResult (*)(uint8_t* ls, spu::Context& regs, renjob::ChanHost& host);

enum ModuleFlags : uint32_t {
    kNoExecutor = 1u << 0,
    kNeedsSpuNumber = 1u << 1,
    kSerial = 1u << 2,
    kTexTable = 1u << 3,
    kOptIn = 1u << 4,
};

struct ModuleInfo {
    uint32_t ea = 0;
    const char* name = "";
    Body semantic = nullptr;
    WholeFn whole = nullptr;
    uint32_t whole_entry = 0;
    uint32_t state_lo = 0;
    uint32_t state_hi = 0;
    uint32_t flags = 0;
};

void register_module(const ModuleInfo& m);
struct Registrar {
    explicit Registrar(const ModuleInfo& m) { register_module(m); }
};
const ModuleInfo* find_module(uint32_t ea);
Tier tier_for(uint32_t module_ea);
bool module_enabled(uint32_t module_ea);

bool enabled();
uint32_t verify_every();

struct Completion {
    std::atomic<int32_t> pending{0};
    std::atomic<uint32_t> wake{0};
};

void submit(JobDesc* const* jobs, uint32_t n, Completion* done);
bool help_one(int site);
void help_until(const Completion& c, int site);
void help_for(int site, uint32_t max_us);
void wait_idle(const Completion& c);
void notify();

struct Consumer {
    virtual const char* name() const = 0;
    virtual bool has_work() = 0;
    virtual bool try_run(JobCtx& ctx) = 0;

protected:
    ~Consumer() = default;
};
void add_consumer(Consumer* c);

bool run_job(const JobDesc& job, JobCtx& ctx);
bool run_on_executor(const JobDesc& job, JobCtx& ctx);
Tier last_tier(const JobCtx& ctx);

namespace verify {
bool want(uint32_t module_ea);
struct Outcome {
    bool ran = false;
    bool same = false;
    uint32_t diff_bytes = 0;
    uint32_t extra_bytes = 0;
    uint32_t first_ea = 0;
};
Outcome check(const JobDesc& job, JobCtx& ctx, Tier candidate);
}

namespace wws {
bool on_submit(ppu::Context& ctx);
bool on_single(ppu::Context& ctx);
bool on_push(ppu::Context& ctx);
bool on_kick(ppu::Context& ctx);
bool on_wait(ppu::Context& ctx);
bool on_barrier(ppu::Context& ctx);
bool on_barrier16(ppu::Context& ctx);
bool on_switch(ppu::Context& ctx);
bool on_help_site(ppu::Context& ctx);
void install(void (*hook)(uint32_t addr, bool (*guard)(ppu::Context&)));
}

bool spu_idle_donate(uint32_t spu_slot);
void spu_thread_started(uint32_t spu_slot);

}
