#include "core.h"

#include "ppu/context.h"
#include "runtime/critpath.h"
#include "runtime/log.h"
#include "runtime/memory.h"

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <immintrin.h>

namespace jobsys {

namespace {

constexpr uint32_t kMaxLists = 32;

struct ListState {
    std::atomic<uint32_t> obj{0};
    Completion done;
    std::atomic<uint32_t> gate{0};
    std::atomic<bool> need_kick{false};
    std::atomic<bool> passthru{false};
};

ListState g_lists[kMaxLists];

ListState* list_state(uint32_t obj, bool create) {
    for (ListState& l : g_lists)
        if (l.obj.load(std::memory_order_acquire) == obj) return &l;
    if (!create) return nullptr;
    static std::mutex m;
    std::lock_guard lk(m);
    for (ListState& l : g_lists)
        if (l.obj.load(std::memory_order_acquire) == obj) return &l;
    for (ListState& l : g_lists) {
        uint32_t zero = 0;
        if (l.obj.compare_exchange_strong(zero, obj)) return &l;
    }
    return nullptr;
}

struct Capture {
    bool active = false;
    bool real_push = false;
    uint32_t site = 0;
    std::vector<JobDesc> jobs;
};
thread_local Capture t_cap;

std::atomic<uint64_t> g_seq{0};

void note_fallback(uint32_t mod, Decode d, uint64_t header) {
    stats().fallback.fetch_add(1, std::memory_order_relaxed);
    if (mod) mod_stat(mod).fallback.fetch_add(1, std::memory_order_relaxed);
    static std::mutex m;
    static std::vector<std::pair<uint32_t, uint32_t>> seen;
    std::lock_guard lk(m);
    for (auto& [a, b] : seen)
        if (a == mod && b == static_cast<uint32_t>(d)) return;
    seen.push_back({mod, static_cast<uint32_t>(d)});
    std::string words;
    uint32_t ea = static_cast<uint32_t>(header), n = static_cast<uint32_t>(header >> 48);
    for (uint32_t off = 0; off + 8 <= n && off < 0x100 && vm::range_mapped(ea, n); off += 8) {
        char b[24];
        std::snprintf(b, sizeof b, " %08x%08x", vm::read32(ea + off), vm::read32(ea + off + 4));
        words += b;
    }
    LOG_INFO("jobsys", "left on the WWS list: module 0x%06x header 0x%016llx (%s, tier %s, enabled %d):%s", mod, static_cast<unsigned long long>(header), decode_name(d), tier_name(tier_for(mod)), module_enabled(mod) ? 1 : 0, words.c_str());
}

bool accept(const JobDesc& j) {
    if (!module_enabled(j.module_ea)) return false;
    Tier t = tier_for(j.module_ea);
    if (t == Tier::Wws) return false;
    const ModuleInfo* m = find_module(j.module_ea);
    if (!j.only_code() && !(m && m->semantic && t == Tier::Semantic)) return false;
    return true;
}

}

namespace wws {

bool on_submit(ppu::Context& ctx) {
    if (!cfg().on) return false;
    uint32_t mod = static_cast<uint32_t>(ctx.gpr[5]);
    if (!module_enabled(mod) || tier_for(mod) == Tier::Wws) return false;
    t_cap.active = true;
    t_cap.real_push = false;
    t_cap.site = static_cast<uint32_t>(ctx.lr);
    t_cap.jobs.clear();
    return false;
}

bool on_single(ppu::Context& ctx) {
    if (!cfg().on) return false;
    if (!t_cap.active) {
        t_cap.active = true;
        t_cap.real_push = false;
        t_cap.site = static_cast<uint32_t>(ctx.lr);
        t_cap.jobs.clear();
    }
    return false;
}

bool on_push(ppu::Context& ctx) {
    if (!t_cap.active) return false;
    JobDesc j;
    uint64_t header = ctx.gpr[5];
    uint32_t list_obj = static_cast<uint32_t>(ctx.gpr[4]);
    Decode d = decode(header, list_obj, j);
    ListState* ls = d == Decode::Ok ? list_state(list_obj, true) : nullptr;
    if (d != Decode::Ok || !ls || ls->passthru.load(std::memory_order_relaxed) || !accept(j)) {
        t_cap.real_push = true;
        note_fallback(j.module_ea, d, header);
        return false;
    }
    j.gate_ea = ls->gate.load(std::memory_order_relaxed);
    j.submit_lr = t_cap.site;
    j.seq = g_seq.fetch_add(1, std::memory_order_relaxed);
    t_cap.jobs.push_back(j);
    return true;
}

bool on_kick(ppu::Context& ctx) {
    if (!t_cap.active) return false;
    t_cap.active = false;
    uint32_t obj = static_cast<uint32_t>(ctx.gpr[3]);
    ListState* ls = list_state(obj, !t_cap.jobs.empty());
    bool need = ls && ls->need_kick.exchange(false);
    if (t_cap.jobs.empty()) return false;
    if (!ls) {
        LOG_ERROR("jobsys", "too many job lists, %zu jobs lost", t_cap.jobs.size());
        return false;
    }
    uint64_t now = __rdtsc();
    std::vector<JobDesc*> ptrs;
    ptrs.reserve(t_cap.jobs.size());
    for (JobDesc& j : t_cap.jobs) {
        if (j.list_obj != obj) j.list_obj = obj;
        j.t_submit = now;
        ptrs.push_back(&j);
    }
    submit(ptrs.data(), static_cast<uint32_t>(ptrs.size()), &ls->done);
    stats().captured.fetch_add(t_cap.jobs.size(), std::memory_order_relaxed);
    t_cap.jobs.clear();
    return !(t_cap.real_push || need);
}

bool on_wait(ppu::Context& ctx) {
    if (!cfg().on) return false;
    uint32_t obj = static_cast<uint32_t>(ctx.gpr[3]);
    ListState* ls = list_state(obj, false);
    if (!ls) return false;
    ls->gate.store(0, std::memory_order_relaxed);
    ls->passthru.store(false, std::memory_order_relaxed);
    if (ls->done.pending.load(std::memory_order_acquire) <= 0) return false;
    uint64_t t0 = __rdtsc();
    help_until_list(ls->done, obj);
    uint64_t t1 = __rdtsc();
    if (critpath::g_on) critpath::emit_wait(critpath::K_WWS_WAIT, t0, t1, obj, 3, 0x705680);
    return false;
}

bool on_barrier(ppu::Context& ctx) {
    if (!cfg().on) return false;
    uint32_t obj = static_cast<uint32_t>(ctx.gpr[4]);
    uint32_t ea = static_cast<uint32_t>(ctx.gpr[5]);
    ListState* ls = list_state(obj, false);
    if (!ls) return false;
    if (cfg().ordered) {
        ls->gate.store(ea, std::memory_order_relaxed);
        ls->need_kick.store(true, std::memory_order_relaxed);
        stats().gated.fetch_add(1, std::memory_order_relaxed);
    } else {
        help_until_list(ls->done, obj);
        ls->need_kick.store(true, std::memory_order_relaxed);
    }
    return false;
}

bool on_barrier16(ppu::Context& ctx) {
    if (!cfg().on) return false;
    uint32_t obj = static_cast<uint32_t>(ctx.gpr[3]) - 0x24;
    ListState* ls = list_state(obj, false);
    if (!ls) return false;
    if (ls->done.pending.load(std::memory_order_acquire) > 0) {
        stats().ordered_waits.fetch_add(1, std::memory_order_relaxed);
        help_until_list(ls->done, obj);
    }
    ls->need_kick.store(true, std::memory_order_relaxed);
    ls->passthru.store(true, std::memory_order_relaxed);
    return false;
}

bool on_switch(ppu::Context& ctx) { return on_barrier16(ctx); }

bool on_help_site(ppu::Context& ctx) {
    if (!cfg().on || !cfg().help_sites) return false;
    (void)ctx;
    help_for(4, 600);
    return false;
}

void install(void (*hook)(uint32_t addr, bool (*guard)(ppu::Context&))) {
    if (!cfg().on) return;
    hook(0x3e13e0, on_submit);
    hook(0x3e16a8, on_single);
    hook(0x49e540, on_push);
    hook(0x49ce50, on_kick);
    hook(0x49e368, on_wait);
    hook(0x49e4e0, on_barrier);
    hook(0x49e1d4, on_barrier16);
    hook(0x49e10c, on_switch);
    if (cfg().help_sites) {
        hook(0x3d3794, on_help_site);
        hook(0x49b85c, on_help_site);
        hook(0x49b76c, on_help_site);
        hook(0x41cbdc, on_help_site);
        hook(0x44ca48, on_help_site);
    }
}

}

}
