#include "core.h"
#include "verify.h"

#include "runtime/log.h"
#include "runtime/reservation.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include <immintrin.h>

namespace jobsys {

Stats& stats() {
    static Stats s;
    return s;
}

double tsc_hz() {
    static const double hz = [] {
        auto c0 = std::chrono::steady_clock::now();
        uint64_t t0 = __rdtsc();
        Sleep(30);
        return double(__rdtsc() - t0) / std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
    }();
    return hz;
}

namespace {

ModStat g_mods[64];
std::mutex g_mods_m;

}

ModStat& mod_stat(uint32_t ea) {
    for (ModStat& m : g_mods)
        if (m.ea == ea) return m;
    std::lock_guard lk(g_mods_m);
    for (ModStat& m : g_mods)
        if (m.ea == ea) return m;
    for (ModStat& m : g_mods) {
        if (!m.ea) {
            m.ea = ea;
            return m;
        }
    }
    return g_mods[63];
}

namespace {

void stats_thread() {
    double hz = tsc_hz();
    Stats& s = stats();
    uint64_t lj = 0, lh = 0, ld = 0, lws = 0, lwj = 0, lwn = 0, lwc = 0, lcap = 0, lfb = 0, lres = 0, lpark = 0, lslow = 0;
    uint64_t lt[4] = {}, ltc[4] = {};
    uint64_t lm[64] = {}, lmc[64] = {}, lmf[64] = {};
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        uint64_t j = s.jobs.load(), h = s.helped.load(), d = s.donated.load(), ws = s.wsites.load(), wj = s.wsite_jobs.load(), wn = s.wait_n.load(), wc = s.wait_cyc.load();
        uint64_t cap = s.captured.load(), fb = s.fallback.load(), res = s.resumed.load(), park = s.park_n.load(), slow = s.slow_dma.load();
        uint64_t mj = s.max_job_cyc.exchange(0), mw = s.max_wait_cyc.exchange(0);
        std::string tiers;
        for (int k = 0; k < 4; k++) {
            uint64_t t = s.by_tier[k].load(), tc = s.tier_cycles[k].load();
            if (t != lt[k]) {
                char b[64];
                std::snprintf(b, sizeof b, " %s %llu (%.0f us)", tier_name(static_cast<Tier>(k)), (unsigned long long)(t - lt[k]), double(tc - ltc[k]) / hz * 1e6 / double(t - lt[k]));
                tiers += b;
            }
            lt[k] = t;
            ltc[k] = tc;
        }
        LOG_INFO("jobsys", "5s: %llu jobs [%s ], waiter %llu, donated %llu, sites %llu/%llu jobs, waits %llu %.2f ms/s max %.2f ms, max job %.2f ms, captured %llu, fallback %llu, resumed %llu, slow dma %llu, parks %llu, gated %llu, drains %llu, lock sleeps %llu, poll sleeps %llu, queued %u",
                 (unsigned long long)(j - lj), tiers.c_str(), (unsigned long long)(h - lh), (unsigned long long)(d - ld), (unsigned long long)(ws - lws), (unsigned long long)(wj - lwj), (unsigned long long)(wn - lwn),
                 double(wc - lwc) / hz * 1e3 / 5.0, double(mw) / hz * 1e3, double(mj) / hz * 1e3, (unsigned long long)(cap - lcap), (unsigned long long)(fb - lfb), (unsigned long long)(res - lres), (unsigned long long)(slow - lslow),
                 (unsigned long long)(park - lpark), (unsigned long long)s.gated.load(), (unsigned long long)s.ordered_waits.load(), (unsigned long long)rsv::g_hard_waits.load(), (unsigned long long)s.relax_sleeps.load(), queued());
        std::string mods;
        for (int i = 0; i < 64; i++) {
            ModStat& m = g_mods[i];
            if (!m.ea) continue;
            uint64_t n = m.jobs.load(), c = m.cyc.load(), f = m.fallback.load();
            if (n != lm[i] || f != lmf[i]) {
                const ModuleInfo* mi = find_module(m.ea);
                char b[128];
                std::snprintf(b, sizeof b, " %s:%llu/%.0fus/max%.1fms/wws%llu", mi ? mi->name : std::to_string(m.ea).c_str(), (unsigned long long)(n - lm[i]), n != lm[i] ? double(c - lmc[i]) / hz * 1e6 / double(n - lm[i]) : 0.0,
                              double(m.max_cyc.exchange(0)) / hz * 1e3, (unsigned long long)(f - lmf[i]));
                mods += b;
            }
            lm[i] = n;
            lmc[i] = c;
            lmf[i] = f;
        }
        if (!mods.empty()) LOG_INFO("jobsys", "5s modules:%s", mods.c_str());
        if (cfg().verify) verify::report();
        lj = j;
        lh = h;
        ld = d;
        lws = ws;
        lwj = wj;
        lwn = wn;
        lwc = wc;
        lcap = cap;
        lfb = fb;
        lres = res;
        lpark = park;
        lslow = slow;
    }
}

}

void start_stats() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (cfg().stats) std::thread(stats_thread).detach();
    });
}

}
