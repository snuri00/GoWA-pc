#include "core.h"
#include "mfc.h"
#include "verify.h"

#include "spu/hle.h"
#include "spu/thread.h"
#include "spu/renjob/renjob_chan.h"

#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/reservation.h"

#include <windows.h>

#include <cstring>
#include <mutex>

#include <immintrin.h>

namespace jobsys {

namespace {

constexpr uint32_t kLoadNextJob = 0x17e0;
constexpr uint32_t kPmSize = 0x3000;
constexpr uint32_t kPmLs = 0xa00;
constexpr uint32_t kCmdLs = 0xc00;
constexpr uint32_t kApiLs = 0x1340;
constexpr uint32_t kCodeLs = 0x4000;

void write_be32(uint8_t* p, uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(p, &v, 4);
}

void load_ls(spu::SpuThread& t, uint32_t lsa, const uint8_t* src, uint32_t size) {
    if (std::memcmp(t.ls + lsa, src, size) == 0) return;
    std::memcpy(t.ls + lsa, src, size);
    t.mark_ls(lsa, size);
}

bool trap(spu::SpuThread& t) {
    uint32_t pc = t.ctx.pc;
    if (pc == kLoadNextJob) {
        t.ctx.pc = t.ctx.gpr[0].u32[3] & spu::PC_MASK;
        return true;
    }
    if (pc == 0x16f0 || pc == 0x2fd8 || pc == 0x1760 || pc == 0x1af0 || pc == 0x1b10 || pc == 0x1a90) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 16) LOG_ERROR("jobsys", "job called unsupported JobApi 0x%04x from 0x%05x", pc, t.ctx.gpr[0].u32[3]);
        t.ctx.pc = t.ctx.gpr[0].u32[3] & spu::PC_MASK;
        return true;
    }
    return false;
}

bool single_next(spu::SpuThread&) { return false; }

}

void reset_regs(spu::SpuThread& t, const JobDesc& j) {
    auto& r = t.ctx.gpr;
    for (int k = 0; k < 4; k++) {
        r[0].u32[k] = 0;
        r[3].u32[k] = kApiLs;
        r[4].u32[k] = 0;
        r[5].u32[k] = 0;
        r[6].u32[k] = 0;
    }
    r[0].u32[3] = kJobReturn;
    r[1].u32[3] = 0x3f990;
    r[5].u32[3] = j.entry;
    r[6].u32[3] = 0x1000;
    t.ctx.pc = j.entry & spu::PC_MASK;
    t.ctx.interrupts_enabled = true;
    t.ctx.halted = false;
    t.ctx.irq_check = false;
    t.tag_mask = t.tag_stat = t.stall_stat = t.atomic_stat = 0;
    t.tag_stat_count = t.stall_stat_count = t.atomic_stat_count = false;
    t.events = 0;
    t.event_mask = 0;
    t.event_count = false;
    t.raddr = 0;
}

std::unique_ptr<spu::SpuThread> make_spu(uint32_t index, const char* name) {
    auto t = std::make_unique<spu::SpuThread>();
    t->id = 0x7e000000u | index;
    t->slot = 32 + index;
    t->name = name;
    t->job_mode = true;
    t->job_stop = kJobReturn;
    t->job_next = single_next;
    t->job_trap = trap;
    t->ctx.pc = kJobReturn;
    return t;
}

Executor& thread_executor(uint32_t index) {
    static thread_local std::unique_ptr<Executor> x;
    if (!x) {
        spu::hle::install_builtin();
        x = std::make_unique<Executor>();
        x->index = index;
        x->t = make_spu(index, "jobsys executor");
        x->arena.resize(1 << 20);
    }
    return *x;
}

void prepare_job(Executor& x, const JobDesc& j) {
    spu::SpuThread& t = *x.t;
    static const std::vector<uint8_t> pm = [] {
        std::vector<uint8_t> v(kPmSize);
        std::memcpy(v.data(), vm::ptr(kPmEa), kPmSize);
        return v;
    }();
    load_ls(t, kPmLs, pm.data(), kPmSize);
    for (uint32_t i = 0; i < j.nloads; i++) {
        const Load& l = j.loads[i];
        if (!l.size) continue;
        uint32_t lsa = j.sets[l.set].ls(l.buf);
        if (l.code) {
            load_ls(t, lsa, vm::ptr(l.ea), l.size);
            continue;
        }
        uint32_t lo = 0xffffffffu, hi = 0;
        if (!l.list) {
            rsv::copy_range_if_changed(l.ea, t.ls + lsa, l.size, lo, hi);
            if (hi > lo) t.mark_ls(lsa + lo, hi - lo);
            continue;
        }
        uint32_t cur = lsa;
        for (uint32_t off = 0; off + 8 <= l.size; off += 8) {
            uint32_t ts = vm::read32(l.ea + off) & 0x7fff;
            uint32_t ea = vm::read32(l.ea + off + 4);
            if (ts && cur + (ea & 15) + ts <= spu::LS_SIZE) {
                uint32_t dst = cur | (ea & 15);
                lo = 0xffffffffu;
                hi = 0;
                rsv::copy_range_if_changed(ea, t.ls + dst, ts, lo, hi);
                if (hi > lo) t.mark_ls(dst + lo, hi - lo);
            }
            cur = (cur + ((ts + 15) & ~15u)) & 0x3fff0;
        }
    }
    uint8_t blk[kMaxCmd] = {};
    std::memcpy(blk, j.cmd, j.cmd_size);
    for (uint32_t off = 0; off + 8 <= j.param_off && off + 8 <= j.cmd_size; off += 8) {
        uint32_t hi;
        std::memcpy(&hi, blk + off, 4);
        hi = __builtin_bswap32(hi);
        if (((hi >> 9) & 7) != 5) std::memset(blk + off, 0, 8);
    }
    load_ls(t, kCmdLs, blk, sizeof blk);
    uint8_t api[0xc0] = {};
    const uint32_t tbl[] = {0x16f0, 0x2fd8, 0x1760, 0x17b8, 0x17c0, 0x17e0, 0x1af0, 0x1b10, 0x1a90};
    write_be32(api + 0x00, x.index);
    write_be32(api + 0x04, 0x1f);
    write_be32(api + 0x0c, j.spurs);
    write_be32(api + 0x10, j.wid << 16);
    write_be32(api + 0x14, 1);
    write_be32(api + 0x1c, 0x000201ff);
    for (int k = 0; k < 4; k++) write_be32(api + 0x20 + 4 * k, kCmdLs + j.param_off);
    for (int k = 0; k < 9; k++) write_be32(api + 0x30 + 4 * k, tbl[k]);
    write_be32(api + 0x60, j.list_hdr | 3);
    write_be32(api + 0x64, 0x1ff);
    write_be32(api + 0x6c, 1);
    write_be32(api + 0x80, 3);
    write_be32(api + 0x84, 0xffffffff);
    write_be32(api + 0x88, kPmEa);
    write_be32(api + 0x8c, kPmSize);
    for (int k = 0; k < 4; k++) write_be32(api + 0xa0 + 4 * k, 1);
    write_be32(api + 0xb0, 1);
    write_be32(api + 0xbc, 0x41);
    load_ls(t, kApiLs, api, sizeof api);
    uint8_t tags[0x20] = {};
    write_be32(tags, 0xff);
    load_ls(t, 0x1290, tags, sizeof tags);
    reset_regs(t, j);
    x.last_module = j.module_ea;
}

void emulate(spu::SpuThread& t, spu::Host* host) {
    for (uint32_t k = 0; k < 64; k++) t.clear_snap(k);
    t.mark_all_ls();
    t.exec_host = host;
    t.stop_flag.store(false);
    t.exec();
    t.exec_host = nullptr;
}

void emulate_fast(spu::SpuThread& t, JobCtx& c) {
    if (!cfg().fast_mfc) {
        emulate(t, nullptr);
        return;
    }
    MfcHost h(t, c);
    MfcSpuHost sh(h);
    emulate(t, &sh);
    if (h.st.slow) stats().slow_dma.fetch_add(h.st.slow, std::memory_order_relaxed);
}

WholeResult run_whole_live(Executor& x, const ModuleInfo& m, JobCtx& c, MfcStats* st) {
    spu::SpuThread& t = *x.t;
    MfcHost h(t, c);
    WholeResult r = m.whole(t.ls, t.ctx, h);
    if (st) *st = h.st;
    return r;
}

bool exec_job(Executor& x, const JobDesc& j, Tier tier) {
    spu::SpuThread& t = *x.t;
    const ModuleInfo* m = find_module(j.module_ea);
    JobCtx c;
    c.worker = x.index;
    c.job = &j;
    c.mem = &live_mem();
    c.exec = &x;
    c.ls = t.ls;
    c.regs = &t.ctx;
    x.arena_used = 0;
    if (tier == Tier::Semantic && m && m->semantic) {
        if (m->semantic(j, c)) {
            x.last = Tier::Semantic;
            return true;
        }
        stats().decl[0].fetch_add(1, std::memory_order_relaxed);
        tier = m->whole ? Tier::Whole : Tier::Executor;
    }
    if (tier == Tier::Semantic) tier = m && m->whole ? Tier::Whole : Tier::Executor;
    if (m && (m->flags & kNoExecutor) && !(tier == Tier::Whole && m->whole)) return false;
    prepare_job(x, j);
    if (cfg().poison) verify::poison(t, j.seq, cfg().poison);
    if (tier == Tier::Whole && m && m->whole && (m->whole_entry == 0 || m->whole_entry == j.entry)) {
        MfcStats st;
        WholeResult r = run_whole_live(x, *m, c, &st);
        if (st.slow) stats().slow_dma.fetch_add(st.slow, std::memory_order_relaxed);
        if (r.done) {
            t.ctx.pc = kJobReturn;
            x.last = Tier::Whole;
            return true;
        }
        stats().resumed.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 24) LOG_WARN("jobsys", "whole %s resumed in the emulator at 0x%05x (icall 0x%05x)", m->name, r.fail_pc, r.icall_miss);
        t.ctx.pc = r.fail_pc;
        emulate_fast(t, c);
        x.last = Tier::Executor;
        return true;
    }
    emulate_fast(t, c);
    x.last = Tier::Executor;
    return true;
}

void* JobCtx::slot(uint32_t key, size_t bytes) {
    if (!exec) return nullptr;
    auto& v = exec->slots[key];
    if (v.size() < bytes) v.resize(bytes, 0);
    return v.data();
}

uint8_t* JobCtx::scratch(size_t bytes) {
    if (!exec) return nullptr;
    size_t a = (exec->arena_used + 127) & ~size_t(127);
    if (a + bytes > exec->arena.size()) exec->arena.resize((a + bytes) * 2);
    exec->arena_used = a + bytes;
    return exec->arena.data() + a;
}

Tier last_tier(const JobCtx& ctx) { return ctx.exec ? ctx.exec->last : Tier::Wws; }

bool run_on_executor(const JobDesc& job, JobCtx& ctx) {
    Executor& x = ctx.exec ? *ctx.exec : thread_executor(ctx.worker);
    return exec_job(x, job, Tier::Executor);
}

}
