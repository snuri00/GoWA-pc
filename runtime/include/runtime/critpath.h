#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include <intrin.h>

namespace ppu {
struct Context;
}

namespace critpath {

enum Type : uint8_t {
    EV_FRAME = 1,
    EV_WAIT = 2,
    EV_SYSCALL = 3,
    EV_JOB = 5,
    EV_STORE = 6,
    EV_FLIP = 7,
    EV_KICK = 8,
    EV_PUSH = 9,
};

enum Kind : uint8_t {
    K_NONE = 0,
    K_FRAME,
    K_COUNTER,
    K_BATCH_FINISH,
    K_BATCH_ADD,
    K_BATCH_KICK,
    K_WWS_WAIT,
    K_EDGE_PREV,
    K_GRAPH_POLL,
    K_GRAPH,
    K_RSX,
    K_WWS_PUSH,
    K_EDGE_GUARD,
};

struct Ev {
    uint64_t t0;
    uint64_t t1;
    uint32_t a, b, c, d;
    uint32_t e;
    uint8_t type;
    uint8_t kind;
    uint8_t depth;
    uint8_t pad;
    uint64_t f;
};
static_assert(sizeof(Ev) == 48);

extern bool g_on;
extern std::atomic<uint32_t> g_watch_n;
extern uint32_t g_pm_sig[2];

void init();
void push(const Ev& e);

struct Hook {
    uint32_t addr;
    uint8_t kind;
};
const Hook* hook_table(int& n);

struct Call {
    uint64_t t0 = 0;
    uint32_t a = 0, b = 0, c = 0, e = 0;
    uint32_t sleeps = 0;
    uint32_t watch = 0xFFFFFFFFu;
};
void enter(uint8_t kind, ppu::Context& ctx, Call& c);
void leave(uint8_t kind, ppu::Context& ctx, Call& c);
void emit_wait(uint8_t kind, uint64_t t0, uint64_t t1, uint32_t a, uint32_t b, uint32_t c);
void syscall_done(uint32_t n, uint64_t t0, uint32_t lr);
void flip(uint64_t count);

struct SpuState {
    bool open = false;
    bool wws = false;
    uint32_t src = 0, wid = 0;
    uint64_t start = 0, pick = 0;
    uint32_t last_src = 0, last_wid = 0;
};

void spu_init_sig();
void spu_job_begin(SpuState& s, uint32_t slot, const uint8_t* ls, const uint32_t* line_src, uint32_t pc, bool wws);
void spu_job_end(SpuState& s, uint32_t slot);
void spu_store(const SpuState& s, uint32_t slot, uint32_t ea, uint32_t size);

inline void spu_block(SpuState& s, uint32_t slot, const uint8_t* ls, const uint32_t* line_src, uint32_t pc) {
    if (s.open) {
        if (pc < 0xa00 || (s.wws ? pc == 0x3368 : pc < 0x3000)) spu_job_end(s, slot);
        return;
    }
    if (pc < 0x3000) {
        if (pc == 0xa00) s.pick = __rdtsc();
        return;
    }
    uint32_t w0;
    std::memcpy(&w0, ls + 0xa00, 4);
    bool wws = w0 == g_pm_sig[0] || w0 == g_pm_sig[1];
    if (wws && pc < 0x4000) return;
    spu_job_begin(s, slot, ls, line_src, pc, wws);
}

inline void spu_stored(const SpuState& s, uint32_t slot, uint32_t ea, uint32_t size) {
    if (g_watch_n.load(std::memory_order_relaxed)) spu_store(s, slot, ea, size);
}

}
