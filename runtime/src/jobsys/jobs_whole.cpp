#include "jobs_whole.h"

#include "wj_an_gen.h"
#include "wj_gv_gen.h"
#include "wj_mh_gen.h"
#include "wj_mp_gen.h"
#include "wj_pb_gen.h"
#include "wj_sh_gen.h"
#include "wj_sa_gen.h"
#include "wj_sm_gen.h"
#include "wj_sp_gen.h"

#include <atomic>
#include <cstring>
#include <xmmintrin.h>

namespace wj {

namespace {

using RootFn = void (*)(renjob::xo::V*, uint8_t*, renjob::xo::Ctl&, uint32_t);

struct Entry {
    Info info;
    RootFn root;
};

#define WJ_ENTRY(ns, name) {{name, renjob::wj_##ns::kImgEa, renjob::wj_##ns::kImgSize, renjob::wj_##ns::kEntry, renjob::wj_##ns::kTail, renjob::wj_##ns::kSig, renjob::wj_##ns::kEntrySig}, renjob::wj_##ns::root}

const Entry kJobs[kJobCount] = {
    WJ_ENTRY(pb, "PrimBuilder"), WJ_ENTRY(gv, "GroupVis"), WJ_ENTRY(sp, "SortPrim"), WJ_ENTRY(mp, "MeshPrep"),
    WJ_ENTRY(mh, "MeshHierarchy"), WJ_ENTRY(sh, "Shadows"), WJ_ENTRY(an, "Animation"), WJ_ENTRY(sm, "ScreamMp3"), WJ_ENTRY(sa, "ScreamAux"),
};

#undef WJ_ENTRY

uint32_t rbe32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

struct ChanAsHost final : spu::Host {
    renjob::ChanHost& ch;
    explicit ChanAsHost(renjob::ChanHost& c) : ch(c) {}
    uint32_t read_channel(uint32_t c) override { return ch.rd(c); }
    void write_channel(uint32_t c, uint32_t v) override { ch.wr(c, v); }
    uint32_t channel_count(uint32_t c) override { return ch.cnt(c); }
    bool stop_and_signal(uint32_t) override {
        ch.abort = true;
        return false;
    }
    void halt(uint32_t, uint32_t) override { ch.abort = true; }
};

thread_local spu::Context* t_ctx = nullptr;
thread_local spu::Host* t_host = nullptr;
thread_local renjob::ChanHost* t_chan = nullptr;
std::atomic<uint64_t> g_ext_calls{0}, g_ext_steps{0};

}

uint32_t ext_call(void* R, uint8_t* ls, uint32_t target, uint32_t ret) {
    spu::Context* c = t_ctx;
    if (!c || static_cast<void*>(c->gpr) != R || c->ls != ls || !t_host) return target;
    constexpr uint64_t kBudget = 4u << 20;
    c->pc = target & spu::PC_MASK;
    uint64_t n = 0;
    g_ext_calls.fetch_add(1, std::memory_order_relaxed);
    while (n < kBudget) {
        if (c->pc == ret) break;
        if (c->halted || (t_chan && t_chan->abort)) break;
        n++;
        if (!spu::step(*c, *t_host)) break;
    }
    g_ext_steps.fetch_add(n, std::memory_order_relaxed);
    return c->pc;
}

uint64_t ext_stats(uint64_t& steps) {
    steps = g_ext_steps.exchange(0);
    return g_ext_calls.exchange(0);
}

const Info& info(int job) { return kJobs[job].info; }

int job_by_ea(uint32_t img_ea) {
    for (int i = 0; i < kJobCount; i++)
        if (kJobs[i].info.img_ea == img_ea) return i;
    return -1;
}

int job_in_ls(const uint8_t* ls) {
    uint32_t w0 = rbe32(ls + 0x4000);
    for (int i = 0; i < kJobCount; i++) {
        const uint32_t* h = kJobs[i].info.header;
        if (h[0] != w0) continue;
        bool ok = true;
        for (int k = 1; k < 12 && ok; k++) ok = rbe32(ls + 0x4000 + 4 * k) == h[k];
        if (ok) return i;
    }
    return -1;
}

Result run(int job, uint8_t* ls, spu::Context& c, renjob::ChanHost& h, bool load_next_job, spu::Host* ext_host) {
    ChanAsHost adapter(h);
    spu::Context* prev_ctx = t_ctx;
    spu::Host* prev_host = t_host;
    renjob::ChanHost* prev_chan = t_chan;
    t_ctx = &c;
    t_host = ext_host ? ext_host : &adapter;
    t_chan = &h;
    constexpr unsigned kWant = 0x6000u | 0x8040u;
    unsigned old = _mm_getcsr();
    if ((old & kWant) != kWant) _mm_setcsr(old | kWant);
    const Entry& e = kJobs[job];
    renjob::xo::Ctl ctl;
    ctl.host = &h;
    ctl.ie = &c.interrupts_enabled;
    t_lnj = load_next_job;
    auto* R = reinterpret_cast<renjob::xo::V*>(c.gpr);
    uint32_t at = e.info.entry;
    Result r;
    for (int k = 0; k < 4; k++) {
        ctl.out = false;
        ctl.fail = false;
        e.root(R, ls, ctl, at);
        uint32_t pc = ctl.fail ? ctl.fail_pc : (renjob::xo::pref(R[0]) & 0x3fffc);
        if (pc == e.info.tail && at != e.info.tail && !h.abort) {
            at = e.info.tail;
            continue;
        }
        if (!ctl.fail && pc == kJobReturn) {
            r.done = true;
            break;
        }
        r.fail_pc = pc;
        r.icall_miss = ctl.icall_miss;
        break;
    }
    if (!r.done && !r.fail_pc) r.fail_pc = renjob::xo::pref(R[0]) & 0x3fffc;
    t_lnj = false;
    t_ctx = prev_ctx;
    t_host = prev_host;
    t_chan = prev_chan;
    if ((old & kWant) != kWant) _mm_setcsr(old);
    return r;
}

}
