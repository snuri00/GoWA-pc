#include "natsched.h"
#include "runtime/host.h"

#include "thread.h"

#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/reservation.h"

#ifdef _WIN32
#include <windows.h>
#endif
#include <immintrin.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>

namespace spu::natsched {

namespace {

constexpr uint64_t kTasksetPmHash = 0x7a2a8ea48cc75b9eull;
constexpr uint32_t kSysServiceWid = 0x20;
constexpr uint32_t kMaxSpus = 8;

struct Cfg {
    int aux = 0;
    bool exclusive = false;
    bool have_wids = false;
    uint32_t wids = 0;
    int period_us = 1000;
    int spin_us = 50;
    int sleep_ms = 1;
    int defer_us = 2000;
};

const Cfg& cfg() {
    static const Cfg c = [] {
        Cfg c;
        const char* e = std::getenv("GOWA_NATIVE_SCHED");
        if (!e || !*e || e[0] == '0') return c;
        std::string s = e;
        c.aux = std::atoi(s.c_str());
        if (c.aux <= 0) c.aux = 1;
        c.exclusive = s.find('x') != std::string::npos;
        if (const char* w = std::getenv("GOWA_NATIVE_SCHED_WIDS")) {
            c.have_wids = true;
            std::string l = w;
            size_t p = 0;
            while (p < l.size()) {
                size_t q = l.find(',', p);
                if (q == std::string::npos) q = l.size();
                int wid = std::atoi(l.substr(p, q - p).c_str());
                if (wid >= 0 && wid < 32) c.wids |= 1u << wid;
                p = q + 1;
            }
        }
        if (const char* v = std::getenv("GOWA_NATIVE_SCHED_PERIOD_US")) c.period_us = (std::max)(100, std::atoi(v));
        if (const char* v = std::getenv("GOWA_NATIVE_SCHED_SPIN_US")) c.spin_us = std::atoi(v);
        if (const char* v = std::getenv("GOWA_NATIVE_SCHED_SLEEP_MS")) c.sleep_ms = (std::max)(1, std::atoi(v));
        if (const char* v = std::getenv("GOWA_NATIVE_SCHED_DEFER_US")) c.defer_us = std::atoi(v);
        return c;
    }();
    return c;
}

std::shared_ptr<Group> g_group;
uint32_t g_spurs = 0;
uint32_t g_main = 0;
uint32_t g_aux_mask = 0;
std::atomic<uint32_t> g_allowed{0};
std::atomic<uint64_t> g_guard_hits{0};
std::atomic<uint64_t> g_updates{0};
std::atomic<uint64_t> g_sleeps{0};
std::atomic<uint64_t> g_entries{0};
std::atomic<uint64_t> g_deferred{0};

uint64_t fnv(const uint8_t* p, uint32_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

bool is_taskset_pm(uint32_t addr, uint32_t size) {
    static std::map<uint64_t, bool> cache;
    uint64_t key = (uint64_t(addr) << 32) | size;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    bool r = false;
    if (addr && size && size <= 0x40000 && vm::range_mapped(addr, size)) r = fnv(vm::ptr(addr), size) == kTasksetPmHash;
    cache[key] = r;
    LOG_INFO("natsched", "workload PM 0x%08x size 0x%x: %s", addr, size, r ? "SPURS taskset PM" : "other");
    return r;
}

uint32_t info_addr(uint32_t wid) { return g_spurs + (wid < 16 ? 0xB00 : 0x1000) + (wid & 15) * 0x20; }

struct Saved {
    bool valid = false;
    uint8_t orig[8] = {};
    uint8_t wrote[8] = {};
};

void filter_once(Saved* saved) {
    if (!vm::range_mapped(g_spurs, 0x1200)) return;
    uint32_t enabled = vm::read32(g_spurs + 0xB0);
    uint32_t allowed = 0;
    uint32_t touched = 0;
    for (uint32_t w = 0; w < 32; w++) {
        uint32_t info = info_addr(w);
        uint8_t cur[8];
        std::memcpy(cur, vm::ptr(info + 0x18), 8);
        Saved& s = saved[w];
        bool on = (enabled & (0x80000000u >> w)) != 0;
        if (!on) {
            s.valid = false;
            continue;
        }
        uint32_t addr = vm::read32(info + 4);
        uint32_t size = vm::read32(info + 0x10);
        bool want = on && is_taskset_pm(addr, size) && (!cfg().have_wids || (cfg().wids & (1u << w)));
        if (!s.valid || std::memcmp(cur, s.wrote, 8) != 0) {
            std::memcpy(s.orig, cur, 8);
            s.valid = true;
        }
        uint8_t best = 0;
        for (uint32_t i = 0; i < g_main; i++) {
            uint8_t p = s.orig[i];
            if (p && (!best || p < best)) best = p;
        }
        uint8_t next[8];
        std::memcpy(next, s.orig, 8);
        if (want && best) {
            for (uint32_t i = g_main; i < kMaxSpus; i++) next[i] = (g_aux_mask >> i) & 1 ? best : s.orig[i];
            if (cfg().exclusive) {
                for (uint32_t i = 0; i < g_main; i++) next[i] = 0;
            }
            allowed |= 1u << w;
        } else {
            for (uint32_t i = g_main; i < kMaxSpus; i++) {
                if ((g_aux_mask >> i) & 1) next[i] = 0;
            }
        }
        if (want && best) g_allowed.fetch_or(1u << w, std::memory_order_seq_cst);
        if (std::memcmp(next, cur, 8) != 0) {
            rsv::store_locked(info + 0x18, next, 8);
            for (uint32_t i = 0; i < kMaxSpus; i++) {
                if (next[i] != cur[i]) touched |= 1u << i;
            }
            LOG_INFO("natsched", "wid %u priority %u,%u,%u,%u,%u,%u,%u,%u -> %u,%u,%u,%u,%u,%u,%u,%u", w, cur[0], cur[1], cur[2], cur[3], cur[4], cur[5], cur[6], cur[7], next[0], next[1], next[2],
                     next[3], next[4], next[5], next[6], next[7]);
        }
        std::memcpy(s.wrote, next, 8);
    }
    g_allowed.store(allowed, std::memory_order_seq_cst);
    if (touched) {
        uint8_t ff = 0xFF;
        rsv::store_locked(g_spurs + 0xBD, &ff, 1);
        rsv::store_locked(g_spurs + 0x72, &ff, 1);
        rsv::watch_kick();
        g_updates.fetch_add(1, std::memory_order_relaxed);
    }
}

Saved g_saved[32];

void filter_thread() {
    host::set_thread_name("natsched filter");
    auto next_log = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (;;) {
        filter_once(g_saved);
        std::this_thread::sleep_for(std::chrono::microseconds(cfg().period_us));
        if (std::chrono::steady_clock::now() >= next_log) {
            next_log += std::chrono::seconds(30);
            char b[256];
            status(b, sizeof b);
            LOG_INFO("natsched", "%s", b);
        }
    }
}

}

int aux_count() { return cfg().aux; }

void on_group_start(const std::shared_ptr<Group>& g) {
    if (!cfg().aux || !g || g_group) return;
    const std::string suffix = "CellSpursKernelGroup";
    if (g->name.size() < suffix.size() || g->name.compare(g->name.size() - suffix.size(), suffix.size(), suffix) != 0) return;
    if (g->threads.empty() || !g->threads[0] || g->num >= kMaxSpus) return;
    SpuThread& t0 = *g->threads[0];
    g_spurs = static_cast<uint32_t>(t0.args[1]);
    g_main = g->num;
    uint32_t n = (std::min)(static_cast<uint32_t>(cfg().aux), kMaxSpus - g_main);
    for (uint32_t i = 0; i < n; i++) g_aux_mask |= 1u << (g_main + i);
    g_group = g;
    rsv::g_watch_line.store(g_spurs & ~127u, std::memory_order_seq_cst);
    filter_once(g_saved);
    std::thread(filter_thread).detach();
    std::string base = t0.name;
    while (!base.empty() && base.back() >= '0' && base.back() <= '9') base.pop_back();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = g_main + i;
        auto t = std::make_shared<SpuThread>();
        t->image_entry = t0.image_entry;
        t->segments = t0.segments;
        t->option = t0.option;
        for (int k = 0; k < 4; k++) t->args[k] = t0.args[k];
        t->args[0] = uint64_t(slot) << 32;
        t->name = base + std::to_string(slot);
        t->slot = slot;
        t->order = slot;
        t->group = g.get();
        t->aux = true;
        t->id = (slot << 24) | (g->id & 0xFFFFFF);
        std::memcpy(t->spup, t0.spup, sizeof t->spup);
        t->spuq = t0.spuq;
        g->aux.push_back(t);
        LOG_INFO("natsched", "aux SPURS SPU %u '%s' spurs 0x%08x (%s taskset workloads)", slot, t->name.c_str(), g_spurs, cfg().exclusive ? "exclusive" : "shared");
        t->start();
    }
}

bool pm_entry_allowed(SpuThread& t) {
    uint32_t wid;
    std::memcpy(&wid, t.ls + 0x1dc, 4);
    wid = __builtin_bswap32(wid);
    if (wid == kSysServiceWid) return true;
    if (wid < 32 && (g_allowed.load(std::memory_order_acquire) & (1u << wid))) {
        g_entries.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    uint64_t n = g_guard_hits.fetch_add(1, std::memory_order_relaxed);
    if (n < 16) LOG_WARN("natsched", "aux SPU %u entered workload %u which is not allowed on aux SPUs; exiting to the kernel", t.slot, wid);
    return false;
}

static bool aux_has_work(uint32_t spu) {
    const uint8_t* b = vm::ptr(g_spurs);
    const uint8_t bit = static_cast<uint8_t>(1u << spu);
    if ((b[0x72] | b[0xBD] | b[0xBF] | b[0xCE]) & bit) return true;
    const uint32_t sig = (uint32_t(b[0x70]) << 24) | (uint32_t(b[0x71]) << 16) | (uint32_t(b[0x78]) << 8) | b[0x79];
    const uint32_t flag = (uint32_t(b[0x6C]) << 24) | (uint32_t(b[0x6D]) << 16) | (uint32_t(b[0x6E]) << 8) | b[0x6F];
    uint32_t allowed = g_allowed.load(std::memory_order_acquire);
    while (allowed) {
        uint32_t w = static_cast<uint32_t>(_tzcnt_u32(allowed));
        allowed &= allowed - 1;
        uint32_t j = w & 15;
        uint32_t cur = w < 16 ? b[0x20 + j] & 15 : b[0x20 + j] >> 4;
        uint32_t max = w < 16 ? b[0x50 + j] & 15 : b[0x50 + j] >> 4;
        if (cur >= max) continue;
        uint32_t ready = w < 16 ? b[j] : b[0x10 + j];
        bool signalled = (sig & (0x80000000u >> (w < 16 ? j : 16 + j))) != 0;
        bool flagged = flag == 0 && b[0x77] == w;
        if (signalled || flagged || ready > cur) return true;
    }
    return false;
}

bool aux_wait(SpuThread& t, uint32_t line, uint64_t rtime, const uint8_t* rdata, uint32_t micros) {
    auto t0 = std::chrono::steady_clock::now();
    auto spin_end = t0 + std::chrono::microseconds(cfg().spin_us);
    auto deadline = t0 + std::chrono::microseconds(micros);
    uint32_t wid;
    std::memcpy(&wid, t.ls + 0x1dc, 4);
    const bool idle = cfg().defer_us > 0 && (line & ~127u) == (g_spurs & ~127u) && __builtin_bswap32(wid) == kSysServiceWid;
    auto defer_end = t0 + std::chrono::microseconds(cfg().defer_us);
    for (;;) {
        uint32_t w0 = rsv::g_watch_word.load(std::memory_order_seq_cst);
        auto now = std::chrono::steady_clock::now();
        if (rsv::lost(line, rtime, rdata)) {
            if (!idle || now >= defer_end || aux_has_work(t.slot)) return true;
            g_deferred.fetch_add(1, std::memory_order_relaxed);
        } else {
            if (now >= deadline) return false;
            if (now < spin_end) {
                _mm_pause();
                continue;
            }
        }
        g_sleeps.fetch_add(1, std::memory_order_relaxed);
        rsv::g_watch_sleepers.fetch_add(1, std::memory_order_seq_cst);
        if (w0 == rsv::g_watch_word.load(std::memory_order_seq_cst)) host::wait_on(&rsv::g_watch_word, &w0, sizeof w0, static_cast<uint32_t>(cfg().sleep_ms));
        rsv::g_watch_sleepers.fetch_sub(1, std::memory_order_seq_cst);
    }
}

void status(char* out, size_t size) {
    std::snprintf(out, size, "aux %u allowed 0x%08x entries %llu deferred %llu guard %llu updates %llu sleeps %llu kicks %u", static_cast<unsigned>(g_group ? g_group->aux.size() : 0), g_allowed.load(),
                  static_cast<unsigned long long>(g_entries.load()), static_cast<unsigned long long>(g_deferred.load()),
                  static_cast<unsigned long long>(g_guard_hits.load()), static_cast<unsigned long long>(g_updates.load()), static_cast<unsigned long long>(g_sleeps.load()),
                  rsv::g_watch_word.load());
}

}
