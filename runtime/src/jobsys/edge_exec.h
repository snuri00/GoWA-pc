#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace spu {
struct SpuThread;
struct Context;
namespace hle {
struct Function;
}
}

namespace jsedge {

constexpr uint32_t kEdgeEa = 0x737580;
constexpr uint32_t kAbEa = 0x7a7200;
constexpr uint32_t kCrEa = 0x71c200;
constexpr uint32_t kPmEa = 0x7a7b00;
constexpr uint32_t kJobRet = 0x3368;
constexpr uint32_t kApiLs = 0x1340;
constexpr uint32_t kCmdLs = 0xc00;
constexpr uint32_t kEntryLs = 0xad98;

struct BufSet {
    uint32_t first_page = 0;
    uint32_t pages = 0;
    uint32_t buffers = 0;
    uint32_t ea = 0;
    uint32_t len = 0;
    uint32_t flags = 0;
    bool used = false;
    bool reserved = false;
};

struct Cmd {
    uint32_t ea = 0;
    uint32_t size = 0;
    uint32_t module = 0;
    uint32_t code_len = 0;
    uint32_t param_off = 0;
    uint32_t run_off = 0;
    uint32_t deps[4] = {};
    uint32_t ndeps = 0;
    BufSet sets[4];
    uint8_t raw[0x100] = {};
};

bool parse(uint32_t cmd_ea, uint32_t cmd_size, Cmd& out);
bool edge_shape(const Cmd& c);

struct JobRef {
    uint32_t hdr = 0;
    uint32_t block = 3;
    uint32_t wid = 0;
    uint32_t index = 0;
    uint32_t spurs = 0;
    uint32_t arr = 0;
};

struct ApiState {
    uint32_t mask[4] = {};
    uint32_t tag[4] = {};
    uint32_t buf = 0;
    const Cmd* cmd = nullptr;
    uint32_t bad_pc = 0;
    uint32_t calls = 0;
};

void prepare_ls(uint8_t* ls, spu::Context& ctx, const JobRef& j, const Cmd& c, uint32_t spu_num, uint32_t buf, bool load_code, bool load_pm);
uint32_t set_base(const Cmd& c, uint32_t set, uint32_t buf);
bool api_call(spu::Context& ctx, ApiState& st);

struct HostCounters {
    uint64_t gets = 0, get_bytes = 0, puts = 0, put_bytes = 0, slow = 0, atomics = 0, dec_reads = 0, label_gets = 0, other_reads = 0;
};

class Executor {
public:
    explicit Executor(uint32_t spu_num);
    ~Executor();
    bool run_edge(const JobRef& j, const Cmd& c, bool reference);
    spu::SpuThread& thread() { return *t; }
    uint32_t spu_num;
    uint32_t toggle = 0;
    uint32_t code_ea = 0;
    bool pm_loaded = false;
    ApiState api;
    HostCounters hc;
    uint32_t fail_pc = 0;
    uint64_t dispatches = 0, fallbacks = 0;
    std::vector<const spu::hle::Function*> hcache;
    std::vector<uint8_t> hvalid;
    std::unique_ptr<spu::SpuThread> t;
};

struct Sandbox {
    std::map<uint32_t, uint8_t> writes;
    uint32_t atomics = 0;
    bool abort = false;
    uint32_t abort_ch = 0;
};

bool run_sandboxed(spu::SpuThread& shadow, ApiState& api, Sandbox& sb);

}
