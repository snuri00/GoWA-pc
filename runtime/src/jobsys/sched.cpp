#include "core.h"
#include "verify.h"

#include "runtime/critpath.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/placement.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <immintrin.h>

namespace crash {
void profile_thread(void* native, const char* name);
}

namespace jobsys {

namespace {

struct Item {
    JobDesc d;
    Completion* c = nullptr;
};

std::mutex g_m;
std::deque<Item> g_q;
std::atomic<uint32_t> g_qn{0};
std::atomic<uint32_t> g_wake{0};
std::atomic<int> g_parked{0};
Consumer* g_cons[8] = {};
std::atomic<int> g_ncons{0};
std::atomic<int> g_helper_index{int(kWaiterIndex)};

enum class Pop { Got, Empty, Blocked };

bool gate_open(uint32_t ea) {
    if (!ea) return true;
    std::atomic_thread_fence(std::memory_order_acquire);
    return vm::read16(ea) == 0;
}

Pop pop(Item& out, uint32_t only_list = 0) {
    if (!g_qn.load(std::memory_order_acquire)) return Pop::Empty;
    std::lock_guard lk(g_m);
    if (g_q.empty()) return Pop::Empty;
    uint32_t closed_list = 0, closed_gate = 0;
    bool any = false;
    for (auto it = g_q.begin(); it != g_q.end(); ++it) {
        if (only_list && it->d.list_obj != only_list) continue;
        any = true;
        if (it->d.gate_ea) {
            if (closed_list == it->d.list_obj && closed_gate == it->d.gate_ea) continue;
            if (!gate_open(it->d.gate_ea)) {
                closed_list = it->d.list_obj;
                closed_gate = it->d.gate_ea;
                continue;
            }
            uint32_t g = it->d.gate_ea, l = it->d.list_obj;
            for (auto& o : g_q)
                if (o.d.gate_ea == g && o.d.list_obj == l) o.d.gate_ea = 0;
        }
        out = std::move(*it);
        g_q.erase(it);
        g_qn.fetch_sub(1, std::memory_order_acq_rel);
        return Pop::Got;
    }
    return any ? Pop::Blocked : Pop::Empty;
}

void complete(Completion* c) {
    if (!c) return;
    if (c->pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        c->wake.fetch_add(1, std::memory_order_release);
        WakeByAddressAll(&c->wake);
    }
}

uint64_t now() { return __rdtsc(); }

void atomic_max(std::atomic<uint64_t>& a, uint64_t v) {
    uint64_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

void run_item(Executor& x, Item& it) {
    const JobDesc& j = it.d;
    uint64_t t0 = now();
    Tier tier = tier_for(j.module_ea);
    if (tier == Tier::Wws) tier = Tier::Executor;
    unsigned csr = _mm_getcsr();
    _mm_setcsr((csr & ~0x6000u) | 0x6000u | 0x8040u);
    if (verify::want(j.module_ea)) verify::run_verified(x, j, tier);
    else exec_job(x, j, tier);
    _mm_setcsr(csr);
    for (uint32_t i = 0; i < j.ndeps; i++) dep_decrement(j.deps[i], j.spurs);
    uint64_t dt = now() - t0;
    Stats& s = stats();
    s.jobs.fetch_add(1, std::memory_order_relaxed);
    uint32_t ti = static_cast<uint32_t>(x.last) & 3;
    s.by_tier[ti].fetch_add(1, std::memory_order_relaxed);
    s.tier_cycles[ti].fetch_add(dt, std::memory_order_relaxed);
    atomic_max(s.max_job_cyc, dt);
    ModStat& ms = mod_stat(j.module_ea);
    ms.jobs.fetch_add(1, std::memory_order_relaxed);
    ms.cyc.fetch_add(dt, std::memory_order_relaxed);
    atomic_max(ms.max_cyc, dt);
    complete(it.c);
    if (g_qn.load(std::memory_order_relaxed) && g_parked.load(std::memory_order_relaxed)) {
        g_wake.fetch_add(1, std::memory_order_release);
        WakeByAddressAll(&g_wake);
    }
}

bool try_consumers(JobCtx& c) {
    int n = g_ncons.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++) {
        Consumer* k = g_cons[i];
        if (k && k->has_work() && k->try_run(c)) return true;
    }
    return false;
}

bool consumers_busy() {
    int n = g_ncons.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++)
        if (g_cons[i] && g_cons[i]->has_work()) return true;
    return false;
}

void worker_main(int k) {
    wchar_t w[64];
    swprintf(w, 64, L"jobsys worker %d", k);
    SetThreadDescription(GetCurrentThread(), w);
    const Config& c = cfg();
    int lp = -1;
    if (k < static_cast<int>(c.cpus.size())) {
        lp = c.cpus[k];
    } else {
        std::vector<int> l = placement::role_lps("spu" + std::to_string(5 - (k % 6)));
        if (l.size() == 1) lp = l[0];
    }
    if (lp >= 0 && lp < 64) SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << lp);
    if (c.prio) SetThreadPriority(GetCurrentThread(), c.prio);
    set_relax_hard(c.prio > 0);
    if (k == 0 && std::getenv("GOWA_PROFILE_JOBSYS")) {
        HANDLE h = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
        crash::profile_thread(h, "jobsys 0");
    }
    LOG_INFO("jobsys", "worker %d on lp %d prio %d", k, lp, c.prio);
    Executor& x = thread_executor(static_cast<uint32_t>(k));
    JobCtx cc;
    cc.worker = x.index;
    cc.exec = &x;
    cc.mem = &live_mem();
    Item it;
    for (;;) {
        Pop p = pop(it);
        if (p == Pop::Got) {
            run_item(x, it);
            continue;
        }
        if (try_consumers(cc)) continue;
        bool more = false;
        for (int i = 0; i < 128; i++) {
            if (p == Pop::Empty && g_qn.load(std::memory_order_relaxed)) {
                more = true;
                break;
            }
            _mm_pause();
        }
        if (more) continue;
        uint32_t seen = g_wake.load(std::memory_order_acquire);
        g_parked.fetch_add(1, std::memory_order_acq_rel);
        if (p == Pop::Empty && g_qn.load(std::memory_order_acquire)) {
            g_parked.fetch_sub(1, std::memory_order_acq_rel);
            continue;
        }
        if (p == Pop::Blocked || consumers_busy()) {
            WaitOnAddress(&g_wake, &seen, sizeof seen, 1);
        } else if (!g_qn.load(std::memory_order_acquire)) {
            WaitOnAddress(&g_wake, &seen, sizeof seen, 4);
        }
        g_parked.fetch_sub(1, std::memory_order_acq_rel);
        stats().park_n.fetch_add(1, std::memory_order_relaxed);
    }
}

Executor& helper_executor() {
    static thread_local Executor* x = nullptr;
    if (!x) {
        int idx = g_helper_index.fetch_sub(1);
        if (idx < 20) idx = 20;
        x = &thread_executor(static_cast<uint32_t>(idx));
    }
    return *x;
}

}

void start_workers() {
    static std::once_flag once;
    std::call_once(once, [] {
        const Config& c = cfg();
        LOG_INFO("jobsys", "native job system on: %d workers (prio %d), help %d, sites %d, donate %d, verify every %u, cpus %d", c.workers, c.prio, c.help ? 1 : 0, c.help_sites ? 1 : 0, c.donate ? 1 : 0, c.verify,
                 placement::logical_cpus());
        for (int k = 0; k < c.workers; k++) std::thread(worker_main, k).detach();
        start_stats();
    });
}

void submit(JobDesc* const* jobs, uint32_t n, Completion* done) {
    if (!n) return;
    start_workers();
    if (done) done->pending.fetch_add(static_cast<int32_t>(n), std::memory_order_acq_rel);
    {
        std::lock_guard lk(g_m);
        for (uint32_t i = 0; i < n; i++) {
            Item it;
            it.d = *jobs[i];
            it.c = done;
            g_q.push_back(std::move(it));
        }
        g_qn.fetch_add(n, std::memory_order_acq_rel);
    }
    g_wake.fetch_add(1, std::memory_order_release);
    if (g_parked.load(std::memory_order_acquire)) WakeByAddressAll(&g_wake);
}

void notify() {
    g_wake.fetch_add(1, std::memory_order_release);
    if (g_parked.load(std::memory_order_acquire)) WakeByAddressAll(&g_wake);
}

void add_consumer(Consumer* c) {
    static std::mutex m;
    std::lock_guard lk(m);
    int n = g_ncons.load();
    if (n >= 8) return;
    g_cons[n] = c;
    g_ncons.store(n + 1, std::memory_order_release);
    start_workers();
}

bool help_list(uint32_t list) {
    if (!cfg().help) return false;
    Item it;
    if (pop(it, list) != Pop::Got) return false;
    Executor& x = helper_executor();
    run_item(x, it);
    stats().helped.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool help_one(int site) {
    (void)site;
    return help_list(0);
}

void help_for(int site, uint32_t max_us) {
    if (!cfg().help || !g_qn.load(std::memory_order_relaxed)) return;
    uint64_t t0 = now();
    uint64_t lim = static_cast<uint64_t>(tsc_hz() * 1e-6 * max_us);
    uint32_t n = 0;
    while (now() - t0 < lim && help_one(site)) n++;
    if (n) {
        stats().wsites.fetch_add(1, std::memory_order_relaxed);
        stats().wsite_jobs.fetch_add(n, std::memory_order_relaxed);
    }
}

void help_until_list(const Completion& c, uint32_t list) {
    if (c.pending.load(std::memory_order_acquire) <= 0) return;
    uint64_t t0 = now();
    uint32_t spins = 0;
    while (c.pending.load(std::memory_order_acquire) > 0) {
        if (help_list(list)) {
            spins = 0;
            continue;
        }
        uint32_t seen = c.wake.load(std::memory_order_acquire);
        if (c.pending.load(std::memory_order_acquire) <= 0) break;
        if (++spins < 64) {
            _mm_pause();
        } else if (spins < 160) {
            SwitchToThread();
        } else {
            WaitOnAddress(const_cast<std::atomic<uint32_t>*>(&c.wake), &seen, sizeof seen, 1);
        }
    }
    uint64_t dt = now() - t0;
    Stats& s = stats();
    s.wait_n.fetch_add(1, std::memory_order_relaxed);
    s.wait_cyc.fetch_add(dt, std::memory_order_relaxed);
    atomic_max(s.max_wait_cyc, dt);
}

void help_until(const Completion& c, int site) {
    (void)site;
    help_until_list(c, 0);
}

void wait_idle(const Completion& c) {
    uint32_t spins = 0;
    while (c.pending.load(std::memory_order_acquire) > 0) {
        uint32_t seen = c.wake.load(std::memory_order_acquire);
        if (c.pending.load(std::memory_order_acquire) <= 0) break;
        if (++spins < 64) _mm_pause();
        else WaitOnAddress(const_cast<std::atomic<uint32_t>*>(&c.wake), &seen, sizeof seen, 1);
    }
}

bool run_job(const JobDesc& job, JobCtx& ctx) {
    Executor& x = ctx.exec ? *ctx.exec : thread_executor(ctx.worker);
    return exec_job(x, job, tier_for(job.module_ea));
}

bool spu_idle_donate(uint32_t spu_slot) {
    if (!cfg().donate || !g_qn.load(std::memory_order_relaxed)) return false;
    Item it;
    if (pop(it) != Pop::Got) return false;
    Executor& x = thread_executor(16 + spu_slot);
    run_item(x, it);
    stats().donated.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void spu_thread_started(uint32_t) {}

uint32_t queued() { return g_qn.load(std::memory_order_relaxed); }

}
