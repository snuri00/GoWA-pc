#include "runtime/placement.h"
#include "spu/aot.h"
#include "hle.h"
#include "edge/edge_gpu.h"
#include "thread.h"
#include "../lv2/event.h"
#include "../lv2/sync.h"
#include "ppu/ops.h"
#include "runtime/log.h"
#include "runtime/trace.h"
#include "runtime/memory.h"
#include "runtime/reservation.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <thread>
#include <mutex>
#include <map>
#include <unordered_map>
#include <string>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace crash {
void profile_thread(void* native, const char* name);
}

namespace spu {

int g_edge_direct = [] {
    const char* e = std::getenv("GOWA_EDGE_DIRECT");
    return e ? std::atoi(e) : 0;
}();

namespace {
std::atomic<uint64_t> g_ev_reads{0}, g_ev_waits{0}, g_ev_wait_us{0}, g_getllar{0}, g_putllc_ok{0}, g_putllc_fail{0}, g_getllar_same{0};

enum : uint32_t {
    EV_LR = 0x400, EV_S1 = 0x200, EV_S2 = 0x100, EV_TM = 0x20, EV_SN = 0x2,
};

constexpr uint64_t SPU_USER_EVENT_KEY = 0xFFFFFFFF53505501ull;
constexpr uint64_t SPU_GROUP_RUN_KEY = 0xFFFFFFFF53505500ull;

uint64_t timebase_now() { return ppu::timebase(); }

}

SpuThread::SpuThread() {
    ls = static_cast<uint8_t*>(_aligned_malloc(LS_SIZE + 16, 128));
    std::memset(ls, 0, LS_SIZE + 16);
    ctx.ls = ls;
    ctx.line_epoch = line_epoch.get();
    ctx.epoch = &ls_epoch;
    ctx.all_epoch = &ls_all_epoch;
    size_t blocks = 0;
    compiled_blocks(blocks);
    region_valid.assign(blocks ? blocks : 1, 0);
    ctx.block_valid = region_valid.data();
}

SpuThread::~SpuThread() {
    rsv::park_free(park_slot);
    _aligned_free(ls);
}

template <class Pred>
bool SpuThread::block(std::unique_lock<std::mutex>& lk, Pred pred) {
    if (pred()) return true;
    const uint64_t cp0 = critpath::g_on ? __rdtsc() : 0;
    while (!pred()) {
        if (stop_flag.load(std::memory_order_relaxed)) {
            ctx.halted = true;
            return false;
        }
        waiting.store(3, std::memory_order_relaxed);
        cv.wait_for(lk, std::chrono::microseconds(100));
        waiting.store(0, std::memory_order_relaxed);
    }
    if (cp0) critpath::spu_waited(cp, cp0, critpath::SW_BLOCK);
    return true;
}

uint32_t SpuThread::dec_now() {
    if (dec_frozen) return dec_value;
    return static_cast<uint32_t>(dec_value - (timebase_now() - dec_start));
}

void SpuThread::set_events(uint32_t bits) {
    events |= bits;
    if (event_mask & bits) event_count = true;
}

bool SpuThread::interrupt_pending() {
    if (!ctx.interrupts_enabled) return false;
    uint32_t mask = std::atomic_ref<uint32_t>(event_mask).load(std::memory_order_relaxed);
    if (std::atomic_ref<bool>(event_count).load(std::memory_order_acquire) || ((mask & EV_LR) && raddr)) return true;
    if (!(mask & EV_TM) || dec_frozen) return false;
    return timebase_now() - dec_start > dec_value;
}

void SpuThread::check_interrupt() {
    if (!ctx.interrupts_enabled) return;
    uint32_t mask = std::atomic_ref<uint32_t>(event_mask).load(std::memory_order_relaxed);
    if (!std::atomic_ref<bool>(event_count).load(std::memory_order_acquire) && !((mask & EV_LR) && raddr)) {
        if (!(mask & EV_TM) || dec_frozen) return;
        if (timebase_now() - dec_start <= dec_value) return;
    }
    std::lock_guard lk(m);
    collect_events();
    if (!event_count) return;
    ctx.interrupts_enabled = false;
    irq_taken = true;
    ctx.srr0 = ctx.pc;
    uint32_t w;
    std::memcpy(&w, ls, 4);
    w = __builtin_bswap32(w);
    ctx.pc = (w & 0xFD80007Fu) == 0x30000000u ? ((w >> 5) & 0x3FFFCu) : 0;
}

static const std::vector<std::pair<uint32_t, uint32_t>> g_dma_watch = [] {
    std::vector<std::pair<uint32_t, uint32_t>> v;
    if (const char* e = std::getenv("GOWA_SPU_DMA")) {
        std::string list = e;
        size_t pos = 0;
        while (pos < list.size()) {
            size_t end = list.find(',', pos);
            if (end == std::string::npos) end = list.size();
            std::string item = list.substr(pos, end - pos);
            size_t dash = item.find('-');
            uint32_t lo = static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 16));
            uint32_t hi = dash != std::string::npos ? static_cast<uint32_t>(std::strtoul(item.c_str() + dash + 1, nullptr, 16)) : lo + 1;
            v.push_back({lo, hi});
            pos = end + 1;
        }
    }
    return v;
}();

static bool dma_watched(uint32_t ea, uint32_t size) {
    for (auto& [lo, hi] : g_dma_watch) {
        if (ea < hi && ea + size > lo) return true;
    }
    return false;
}

void SpuThread::log_watched_dma(const char* what, uint32_t cmd, uint32_t ea, uint32_t lsa, uint32_t size, const uint8_t* data) {
    std::string hex;
    char b[4];
    uint32_t n = size < 64 ? size : 64;
    for (uint32_t i = 0; i < n; i++) {
        std::snprintf(b, sizeof b, "%02x", data ? data[i] : vm::ptr(ea)[i]);
        hex += b;
        if ((i & 3) == 3) hex += ' ';
    }
    uint32_t code[4];
    for (int i = 0; i < 4; i++) code[i] = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(ls + 0x4100 + i * 4));
    LOG_INFO("spudma", "thread 0x%x pc 0x%05x %s cmd 0x%02x ea 0x%08x lsa 0x%05x size 0x%x %s code4100 %08x%08x%08x%08x", id, ctx.pc, what, cmd, ea, lsa, size, hex.c_str(), code[0], code[1], code[2], code[3]);
}

static const bool job_stats = std::getenv("GOWA_SPU_JOBS") != nullptr;

void SpuThread::collect_events() {
    if (!dec_frozen) {
        uint64_t elapsed = timebase_now() - dec_start;
        if (elapsed > dec_value) {
            set_events(EV_TM);
            dec_start += 0x100000000ull;
        }
    }
    if ((event_mask & EV_LR) && raddr && !(events & EV_LR)) {
        if (rsv::lost(raddr, rtime, rdata)) {
            set_events(EV_LR);
            raddr = 0;
        }
    }
    if (events & event_mask) event_count = true;
}

void SpuThread::push_snr(uint32_t number, uint32_t value) {
    std::lock_guard lk(m);
    bool orm = (snr_config >> number) & 1;
    bool was = snr_count[number];
    snr[number] = orm && was ? (snr[number] | value) : value;
    snr_count[number] = true;
    if (!was || !orm) set_events(number == 0 ? EV_S1 : EV_S2);
    cv.notify_all(); wake_rsv_waiter();
}

void SpuThread::push_in_mbox(uint32_t value) {
    std::lock_guard lk(m);
    if (in_mbox.size() >= 4) in_mbox.back() = value;
    else in_mbox.push_back(value);
    cv.notify_all(); wake_rsv_waiter();
}

uint32_t SpuThread::pop_out_mbox() {
    std::lock_guard lk(m);
    out_mbox_full = false;
    cv.notify_all(); wake_rsv_waiter();
    return out_mbox;
}

uint32_t SpuThread::channel_count(uint32_t ch) {
    switch (ch) {
    case 21: return 16;
    case 23: return 1;
    case 24: return tag_stat_count;
    case 25: return stall_stat_count;
    case 27: return atomic_stat_count;
    case 9: return 0;
    default: break;
    }
    std::lock_guard lk(m);
    switch (ch) {
    case 0: collect_events(); return event_count ? 1 : 0;
    case 3: return snr_count[0];
    case 4: return snr_count[1];
    case 21: return 16;
    case 23: return 1;
    case 24: return tag_stat_count;
    case 25: return stall_stat_count;
    case 27: return atomic_stat_count;
    case 28: return out_mbox_full ? 0 : 1;
    case 29: return static_cast<uint32_t>(in_mbox.size());
    case 30: return out_intr_full ? 0 : 1;
    case 9: return 0;
    default: return 1;
    }
}

uint32_t SpuThread::read_channel(uint32_t ch) {
    switch (ch) {
    case 8: return dec_now();
    case 12: return tag_mask;
    case 13: return ctx.interrupts_enabled ? 1 : 0;
    case 15: return ctx.srr0;
    case 24:
        if (tag_stat_count) {
            tag_stat_count = false;
            return tag_stat;
        }
        break;
    case 27:
        if (atomic_stat_count) {
            atomic_stat_count = false;
            return atomic_stat;
        }
        break;
    default: break;
    }
    std::unique_lock lk(m);
    switch (ch) {
    case 0: {
        collect_events();
        if (job_stats) {
            g_ev_reads.fetch_add(1, std::memory_order_relaxed);
            if (!event_count) g_ev_waits.fetch_add(1, std::memory_order_relaxed);
        }
        auto wait_start = job_stats && !event_count ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const uint64_t cp0 = critpath::g_on && !event_count ? __rdtsc() : 0;
        while (!event_count) {
            if (stop_flag.load(std::memory_order_relaxed)) {
                ctx.halted = true;
                return 0;
            }
            if ((event_mask & EV_LR) && raddr) {
                uint32_t line = raddr;
                uint64_t rt = rtime;
                uint8_t snap[128];
                std::memcpy(snap, rdata, 128);
                if (park_slot >= 0 && rsv::park_enabled()) {
                    uint32_t w0 = rsv::park_word(park_slot);
                    int64_t limit = -1;
                    if ((event_mask & EV_TM) && !dec_frozen) {
                        uint64_t elapsed = timebase_now() - dec_start;
                        limit = elapsed >= dec_value ? 0 : static_cast<int64_t>((dec_value - elapsed) / 80);
                    }
                    lk.unlock();
                    waiting.store(1, std::memory_order_relaxed);
                    rsv::wait_parked(line, rt, snap, park_slot, w0, limit, stop_flag);
                    waiting.store(0, std::memory_order_relaxed);
                    lk.lock();
                } else {
                    lk.unlock();
                    waiting.store(1, std::memory_order_relaxed);
                    rsv::wait_lost(line, rt, snap, 1000);
                    waiting.store(0, std::memory_order_relaxed);
                    lk.lock();
                }
            } else {
                waiting.store(2, std::memory_order_relaxed);
                cv.wait_for(lk, std::chrono::microseconds(100));
                waiting.store(0, std::memory_order_relaxed);
            }
            collect_events();
        }
        if (cp0) critpath::spu_waited(cp, cp0, critpath::SW_LR);
        if (job_stats && wait_start.time_since_epoch().count()) g_ev_wait_us.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - wait_start).count(), std::memory_order_relaxed);
        event_count = false;
        return events & event_mask;
    }
    case 3:
    case 4: {
        int n = ch - 3;
        if (!block(lk, [&] { return snr_count[n]; })) return 0;
        snr_count[n] = false;
        uint32_t v = snr[n];
        snr[n] = 0;
        return v;
    }
    case 8: return dec_now();
    case 11: return event_mask;
    case 12: return tag_mask;
    case 13: return ctx.interrupts_enabled ? 1 : 0;
    case 15: return ctx.srr0;
    case 24:
        if (!block(lk, [&] { return tag_stat_count; })) return 0;
        tag_stat_count = false;
        return tag_stat;
    case 25: {
        if (!block(lk, [&] { return stall_stat_count; })) return 0;
        stall_stat_count = false;
        uint32_t v = stall_stat;
        stall_stat = 0;
        return v;
    }
    case 27:
        if (!block(lk, [&] { return atomic_stat_count; })) return 0;
        atomic_stat_count = false;
        return atomic_stat;
    case 29: {
        if (!block(lk, [&] { return !in_mbox.empty(); })) return 0;
        uint32_t v = in_mbox.front();
        in_mbox.pop_front();
        return v;
    }
    default:
        LOG_ERROR("spu", "thread 0x%x read of unknown channel %u at 0x%05x", id, ch, ctx.pc);
        return 0;
    }
}

void SpuThread::write_channel(uint32_t ch, uint32_t value) {
    switch (ch) {
    case 14: ctx.srr0 = value & 0x3FFFC; return;
    case 16: mfc_lsa = value; return;
    case 17: return;
    case 18: mfc_eal = value; return;
    case 19: mfc_size = value > 0xFFFF ? 0xFFFF : value; return;
    case 20: mfc_tag = value & 0x1F; return;
    case 21: process_mfc(value & 0xFF); return;
    case 22: tag_mask = value; return;
    case 23:
        tag_stat = tag_mask;
        if (value == 1 && tag_mask == 0) return;
        tag_stat_count = true;
        return;
    default: break;
    }
    std::unique_lock lk(m);
    switch (ch) {
    case 1:
        collect_events();
        event_mask = value;
        if (events & event_mask) event_count = true;
        lk.unlock();
        check_interrupt();
        return;
    case 2:
        collect_events();
        if ((value & EV_TM) && !(event_mask & EV_TM)) {
            dec_value = dec_now();
            dec_frozen = true;
        }
        events &= ~value;
        event_count = (events & event_mask) != 0;
        lk.unlock();
        check_interrupt();
        return;
    case 7:
        collect_events();
        dec_start = timebase_now();
        dec_value = value;
        dec_frozen = false;
        return;
    case 14: ctx.srr0 = value & 0x3FFFC; return;
    case 16: mfc_lsa = value; return;
    case 17: return;
    case 18: mfc_eal = value; return;
    case 19: mfc_size = value > 0xFFFF ? 0xFFFF : value; return;
    case 20: mfc_tag = value & 0x1F; return;
    case 21:
        lk.unlock();
        process_mfc(value & 0xFF);
        return;
    case 22: tag_mask = value; return;
    case 23:
        tag_stat = tag_mask;
        if (value == 1 && tag_mask == 0) return;
        tag_stat_count = true;
        return;
    case 26: {
        uint32_t tag = value & 0x1F;
        auto st = stalled[tag];
        if (!st.active) return;
        stalled[tag].active = false;
        lk.unlock();
        run_list(tag, st.cmd, st.lsa, st.list, st.remaining);
        return;
    }
    case 28:
        if (!block(lk, [&] { return !out_mbox_full; })) return;
        out_mbox = value;
        out_mbox_full = true;
        return;
    case 30: {
        uint32_t code = value >> 24;
        uint32_t data0 = value & 0xFFFFFF;
        if (!out_mbox_full) {
            LOG_ERROR("spu", "thread 0x%x WrOutIntrMbox 0x%08x with empty out mbox", id, value);
            return;
        }
        uint32_t data1 = out_mbox;
        out_mbox_full = false;
        lk.unlock();
        if (code < 128) {
            uint32_t port = code & 63;
            bool reply = code < 64;
            if (reply) {
                std::lock_guard l2(m);
                if (!in_mbox.empty()) {
                    in_mbox.assign(1, static_cast<uint32_t>(lv2::CELL_EBUSY));
                    cv.notify_all(); wake_rsv_waiter();
                    return;
                }
            }
            int64_t r = lv2::CELL_ENOTCONN;
            if (uint32_t q = spup[port]) {
                if (auto queue = lv2::get<lv2::EventQueue>(q)) {
                    r = lv2::push_event(queue, {SPU_USER_EVENT_KEY, id, (uint64_t(port) << 32) | data0, data1});
                }
            }
            LOG_DEBUG("spu", "thread 0x%x %s port %u data0 0x%x data1 0x%x queue 0x%x -> 0x%llx", id, reply ? "send_event" : "throw_event", port, data0, data1,
                      spup[port], static_cast<unsigned long long>(r));
            if (reply) push_in_mbox(static_cast<uint32_t>(r));
            else if (r) LOG_WARN("spu", "thread 0x%x throw_event port %u failed 0x%llx", id, port, static_cast<unsigned long long>(r));
        } else if (code == 128 || code == 192) {
            int64_t r = lv2::event_flag_set_bits(data1, 1ull << (data0 & 63));
            LOG_DEBUG("spu", "thread 0x%x event_flag_set 0x%x bit %u -> 0x%llx", id, data1, data0 & 63, static_cast<unsigned long long>(r));
            if (code == 128) push_in_mbox(static_cast<uint32_t>(r));
        } else {
            LOG_ERROR("spu", "thread 0x%x unknown WrOutIntrMbox code %u", id, code);
        }
        return;
    }
    case 9:
    case 69:
    case 70:
    case 71:
        return;
    default:
        LOG_ERROR("spu", "thread 0x%x write 0x%x to unknown channel %u at 0x%05x", id, value, ch, ctx.pc);
        return;
    }
}

uint8_t* SpuThread::translate(uint32_t ea, uint32_t size, bool write, bool& handled, uint32_t lsa) {
    handled = false;
    if (ea >= 0xF0000000 && group) {
        uint32_t slot_n = (ea - 0xF0000000) / 0x100000;
        uint32_t off = (ea - 0xF0000000) % 0x100000;
        SpuThread* t = group->by_slot(slot_n);
        if (t) {
            if (off + size <= LS_SIZE) return t->ls + off;
            if (write && size == 4 && (off == 0x5400C || off == 0x5C00C)) {
                uint32_t v;
                std::memcpy(&v, ls + (lsa & 0x3FFFF), 4);
                t->push_snr(off == 0x5400C ? 0 : 1, __builtin_bswap32(v));
                handled = true;
                return nullptr;
            }
        }
        LOG_ERROR("spu", "thread 0x%x DMA to unsupported SPU window 0x%08x size 0x%x", id, ea, size);
        handled = true;
        return nullptr;
    }
    return vm::ptr(ea);
}


namespace {
struct RaceCfg {
    uint32_t lo = 0, len = 0;
    double at = 0.0;
    RaceCfg() {
        if (const char* e = std::getenv("GOWA_SPU_RACE")) {
            lo = static_cast<uint32_t>(std::strtoul(e, nullptr, 16));
            const char* plus = std::strchr(e, '+');
            len = plus ? static_cast<uint32_t>(std::strtoul(plus + 1, nullptr, 16)) : 0x1000;
        }
        if (const char* a = std::getenv("GOWA_SPU_RACE_AT")) at = std::atof(a);
    }
};
const RaceCfg& race_cfg() {
    static RaceCfg c;
    return c;
}
std::mutex g_race_lock;
std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> g_race_snap;
std::atomic<int> g_race_logged{0};
}

void race_on_get(uint32_t id, uint32_t ea, uint32_t size, uint32_t pc) {
    const RaceCfg& c = race_cfg();
    if (!c.lo || ea >= c.lo + c.len || ea + size <= c.lo || logging::uptime() < c.at) return;
    std::vector<uint8_t> v(size);
    std::memcpy(v.data(), vm::ptr(ea), size);
    std::lock_guard lk(g_race_lock);
    g_race_snap[{id, ea}] = std::move(v);
}

void race_on_put(uint32_t id, uint32_t ea, uint32_t size, const uint8_t* src, uint32_t pc) {
    const RaceCfg& c = race_cfg();
    if (!c.lo || ea >= c.lo + c.len || ea + size <= c.lo) return;
    std::lock_guard lk(g_race_lock);
    auto it = g_race_snap.find({id, ea});
    if (it == g_race_snap.end() || it->second.size() != size) return;
    const uint8_t* cur = vm::ptr(ea);
    int lost = 0;
    uint32_t first = 0, fold = 0, fcur = 0;
    for (uint32_t i = 0; i + 4 <= size; i += 4) {
        if (std::memcmp(cur + i, it->second.data() + i, 4) != 0 && std::memcmp(src + i, it->second.data() + i, 4) == 0) {
            if (!lost) {
                first = ea + i;
                std::memcpy(&fold, it->second.data() + i, 4);
                std::memcpy(&fcur, cur + i, 4);
            }
            lost++;
        }
    }
    if (lost && g_race_logged.fetch_add(1) < 60) {
        LOG_INFO("spurace", "thread 0x%x pc 0x%05x PUT ea 0x%08x size 0x%x discards %d foreign word updates, first 0x%08x: snapshot 0x%08x current 0x%08x", id, pc, ea, size, lost, first,
                 __builtin_bswap32(fold), __builtin_bswap32(fcur));
    }
    g_race_snap.erase(it);
}

bool put_merge_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("GOWA_SPU_PUT_MERGE");
        return !e || std::atoi(e) != 0;
    }();
    return on;
}

namespace {
struct JobStat {
    uint64_t entries = 0;
    uint64_t insns = 0;
    uint32_t wid = 0;
    uint32_t pc = 0;
};
std::mutex g_job_lock;
std::map<uint32_t, JobStat> g_jobs;
std::atomic<uint64_t> g_interp_insns{0};
std::atomic<uint64_t> g_aot_insns{0};
std::atomic<uint64_t> g_region_insns{0};
std::atomic<uint64_t> g_block_calls{0};
std::atomic<uint64_t> g_cyc_cycles{0};
std::atomic<uint64_t> g_gen_cycles{0};
struct BlockCyc {
    uint64_t cycles = 0;
    uint64_t insns = 0;
    uint64_t calls = 0;
};
std::map<const void*, BlockCyc> g_block_cyc;
std::atomic<uint64_t> g_cyc_insns{0};
std::map<uint32_t, uint64_t> g_interp_pcs;
}

void note_job(uint32_t id, uint32_t wid, uint32_t src, uint32_t pc, int mode, uint32_t prev_src, uint64_t prev_insns) {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(10));
                std::vector<std::pair<uint32_t, JobStat>> v;
                {
                    std::lock_guard lk(g_job_lock);
                    v.assign(g_jobs.begin(), g_jobs.end());
                    for (auto& [k, j] : g_jobs) j.insns = 0;
                }
                std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.insns > b.second.insns; });
                uint64_t total = 0;
                for (auto& e : v) total += e.second.insns;
                std::string line;
                for (size_t i = 0; i < v.size() && i < 16; i++) {
                    char b[80];
                    std::snprintf(b, sizeof b, " [code %08x wid %u %.1f%%]", v[i].first, v[i].second.wid, total ? 100.0 * v[i].second.insns / total : 0.0);
                    line += b;
                }
                LOG_INFO("spujob", "top:%s", line.c_str());
                uint64_t ii = g_interp_insns.exchange(0), ai = g_aot_insns.exchange(0);
                std::vector<std::pair<uint32_t, uint64_t>> pcs;
                {
                    std::lock_guard lk(g_job_lock);
                    pcs.assign(g_interp_pcs.begin(), g_interp_pcs.end());
                    g_interp_pcs.clear();
                }
                std::sort(pcs.begin(), pcs.end(), [](auto& a, auto& b) { return a.second > b.second; });
                std::string hot;
                for (size_t i = 0; i < pcs.size() && i < 12; i++) {
                    char b[48];
                    std::snprintf(b, sizeof b, " %05x:%llu", pcs[i].first, static_cast<unsigned long long>(pcs[i].second));
                    hot += b;
                }
                uint64_t ri = g_region_insns.exchange(0), bc = g_block_calls.exchange(0);
                uint64_t cc = g_cyc_cycles.exchange(0), ci = g_cyc_insns.exchange(0), gc = g_gen_cycles.exchange(0);
                LOG_INFO("spujob", "region share %.1f%%, %.1f insns per dispatch, %.1f cycles per insn, %.1f in generated code", ai ? 100.0 * ri / ai : 0.0, bc ? double(ai) / bc : 0.0, ci ? double(cc) / ci : 0.0, ai ? double(gc) / ai : 0.0);
                LOG_INFO("spujob", "event reads %llu (blocked %llu, %llu us), getllar %llu (unchanged %llu), putllc ok %llu fail %llu", static_cast<unsigned long long>(g_ev_reads.exchange(0)),
                         static_cast<unsigned long long>(g_ev_waits.exchange(0)), static_cast<unsigned long long>(g_ev_wait_us.exchange(0)), static_cast<unsigned long long>(g_getllar.exchange(0)),
                         static_cast<unsigned long long>(g_getllar_same.exchange(0)), static_cast<unsigned long long>(g_putllc_ok.exchange(0)), static_cast<unsigned long long>(g_putllc_fail.exchange(0)));
                std::vector<std::pair<const void*, BlockCyc>> bv;
                {
                    std::lock_guard lk(g_job_lock);
                    bv.assign(g_block_cyc.begin(), g_block_cyc.end());
                    g_block_cyc.clear();
                }
                std::sort(bv.begin(), bv.end(), [](auto& a, auto& b) { return a.second.cycles > b.second.cycles; });
                uint64_t btot = 0;
                for (auto& e : bv) btot += e.second.cycles;
                for (size_t i = 0; i < bv.size() && i < 40; i++) {
                    const CompiledBlock* cb = static_cast<const CompiledBlock*>(bv[i].first);
                    LOG_INFO("spujob", "  block lsa %05x words %u: %.1f%% of cycles, %.1f cycles per insn, %.1f insns per call", cb->lsa, cb->words, btot ? 100.0 * bv[i].second.cycles / btot : 0.0,
                             bv[i].second.insns ? double(bv[i].second.cycles) / bv[i].second.insns : 0.0, bv[i].second.calls ? double(bv[i].second.insns) / bv[i].second.calls : 0.0);
                }
                LOG_INFO("spujob", "interpreted %.2f%% of %llu insns, hot interpreted block starts:%s", (ii + ai) ? 100.0 * ii / double(ii + ai) : 0.0, static_cast<unsigned long long>(ii + ai), hot.c_str());
            }
        }).detach();
    });
    {
        std::lock_guard lk(g_job_lock);
        g_jobs[prev_src].insns += prev_insns;
        JobStat& j = g_jobs[src];
        j.entries++;
        j.wid = wid;
        j.pc = pc;
    }
    if (mode >= 2) LOG_INFO("spujob", "thread 0x%x enters code %08x wid %u pc 0x%05x", id, src, wid, pc);
}

void SpuThread::clear_snap(uint32_t k) {
    DmaSnap& d = dma_snaps[k];
    if (!d.size) return;
    for (uint32_t l = d.lsa >> 7, last = (d.lsa + d.size - 1) >> 7; l <= last && l < snap_line.size(); l++) snap_line[l] &= ~(uint64_t(1) << k);
    d.size = 0;
}

void SpuThread::forget_ls(uint32_t lsa, uint32_t size) {
    if (!size) return;
    uint64_t cand = 0;
    for (uint32_t l = lsa >> 7, last = (lsa + size - 1) >> 7; l <= last && l < snap_line.size(); l++) cand |= snap_line[l];
    while (cand) {
        uint32_t k = static_cast<uint32_t>(__builtin_ctzll(cand));
        cand &= cand - 1;
        DmaSnap& d = dma_snaps[k];
        if (d.size && d.lsa < lsa + size && lsa < d.lsa + d.size) clear_snap(k);
    }
}

void SpuThread::remember_get(uint32_t ea, uint32_t size, uint32_t lsa) {
    forget_ls(lsa, size);
    if (size < 16) return;
    DmaSnap* slot_ptr = nullptr;
    for (DmaSnap& d : dma_snaps) {
        if (!d.size) {
            slot_ptr = &d;
            break;
        }
    }
    if (!slot_ptr) {
        uint32_t table = vm::is_mapped(0x9B2298) ? vm::read32(0x9B2298) : 0;
        uint32_t victim = dma_snap_next;
        for (uint32_t n = 0; table && n < dma_snaps.size(); n++) {
            uint32_t k = (dma_snap_next + n) % dma_snaps.size();
            const DmaSnap& d = dma_snaps[k];
            if (d.ea >= table + 0xA280 || d.ea + d.size <= table) {
                victim = k;
                break;
            }
        }
        clear_snap(victim);
        slot_ptr = &dma_snaps[victim];
        dma_snap_next = (victim + 1) % dma_snaps.size();
    }
    slot_ptr->ea = ea;
    slot_ptr->size = size;
    slot_ptr->lsa = lsa;
    slot_ptr->data.assign(ls + lsa, ls + lsa + size);
    uint64_t bit = uint64_t(1) << (slot_ptr - dma_snaps.data());
    for (uint32_t l = lsa >> 7, last = (lsa + size - 1) >> 7; l <= last && l < snap_line.size(); l++) snap_line[l] |= bit;
}

bool SpuThread::merge_put(uint32_t ea, uint32_t size, uint32_t lsa) {
    merge_hit = false;
    uint64_t cand = (lsa >> 7) < snap_line.size() ? snap_line[lsa >> 7] : 0;
    while (cand) {
        DmaSnap& d = dma_snaps[__builtin_ctzll(cand)];
        cand &= cand - 1;
        if (!d.size || ea < d.ea || ea + size > d.ea + d.size || d.lsa + (ea - d.ea) != lsa) continue;
        uint8_t* base = d.data.data() + (ea - d.ea);
        const uint8_t* src = ls + lsa;
        merge_hit = true;
        if (std::memcmp(vm::ptr(ea), base, size) == 0) {
            std::memcpy(base, src, size);
            return false;
        }
        uint32_t table = vm::is_mapped(0x9B2298) ? vm::read32(0x9B2298) : 0;
        if (table && ea >= table && ea + size <= table + 0xA280 && ((ea - table) & 15) == 0 && (size & 15) == 0) {
            for (uint32_t i = 0; i < size; i += 16) {
                if (std::memcmp(vm::ptr(ea + i), base + i, 16) != 0) continue;
                if (std::memcmp(src + i, base + i, 16) == 0) continue;
                rsv::merge_bits(ea + i, src + i, base + i, 16);
            }
            std::memcpy(base, src, size);
            return true;
        }
        uint32_t i = 0;
        while (i < size) {
            uint32_t n = (std::min)(4u, size - i);
            if (std::memcmp(src + i, base + i, n) == 0) {
                i += n;
                continue;
            }
            uint32_t start = i;
            while (i < size) {
                uint32_t k = (std::min)(4u, size - i);
                if (std::memcmp(src + i, base + i, k) == 0) break;
                i += k;
            }
            uint32_t done = start;
            while (done < i) {
                uint32_t a = ea + done;
                uint32_t chunk = (std::min)(i - done, 128 - (a & 127));
                rsv::merge_bits(a, src + done, base + done, chunk);
                done += chunk;
            }
        }
        std::memcpy(base, src, size);
        return true;
    }
    return false;
}

namespace {

bool dma_plain() {
    static const bool plain = g_dma_watch.empty() && !std::getenv("GOWA_SPU_RACE") && !std::getenv("GOWA_SPU_PUT_FIND") && !std::getenv("GOWA_SPU_PUT_CLEARS");
    return plain;
}

uint32_t ls_word(const uint8_t* ls, uint32_t a) {
    uint32_t v;
    std::memcpy(&v, ls + a, 4);
    return __builtin_bswap32(v);
}

bool edge_loaded(const uint8_t* ls) {
    static const uint32_t guid[4] = {0x43642a02u, 0x42370282u, 0x420c0702u, 0x43499982u};
    for (int i = 0; i < 4; i++)
        if (ls_word(ls, 0x4000 + 4 * i) != guid[i]) return false;
    return ls_word(ls, 0x4020) == 0xc0dec0deu;
}

bool in_ring(const uint8_t* ls, uint32_t ea, uint32_t size) {
    uint64_t end = uint64_t(ea) + size;
    uint32_t a0 = ls_word(ls, 0x10c00), a1 = ls_word(ls, 0x10c08);
    if (a0 >= 0x10000 && a1 > a0 && a1 - a0 <= 0x400000 && ea >= a0 && end <= a1) return true;
    uint32_t b0 = ls_word(ls, 0x10c20), bn = ls_word(ls, 0x10c24);
    return b0 >= 0x10000 && bn && bn <= 0x400000 && ea >= b0 && end <= uint64_t(b0) + bn;
}

bool edge_list(const uint8_t* ls) {
    uint32_t wid = ls_word(ls, 0x1dc);
    return wid >= 19 && wid <= 21;
}

}

std::atomic<uint64_t> g_edge_merge_hits{0};

bool SpuThread::dma_direct(uint32_t cmd, uint32_t lsa, uint32_t ea, uint32_t size) {
    if (!dma_plain() || ea >= 0xF0000000u || ea < 0x10000 || size > 0x4000 || cmd == 0x89) return false;
    if (!vm::is_mapped(ea) || !vm::is_mapped(ea + size - 1)) return false;
    lsa &= 0x3FFFF;
    if (lsa + size > LS_SIZE) return false;
    if (trace::on(trace::DMA)) return false;
    bool put = (cmd & 0xF0) == 0x20 || (cmd & 0xF0) == 0x30 || (cmd & 0xF0) == 0xA0;
    const bool merge = put_merge_enabled();
    if (put) {
        bool merged = merge && merge_put(ea, size, lsa);
        if (merge && merge_hit && edge_loaded(ls) && g_edge_merge_hits.fetch_add(1, std::memory_order_relaxed) < 8) {
            LOG_INFO("spudma", "thread 0x%x Edge job PUT hit a GET snapshot: ea 0x%08x size 0x%x lsa 0x%05x merged %d wid %u", id, ea, size, lsa, merged ? 1 : 0, ls_word(ls, 0x1dc));
        }
        if (merged) return true;
        uint32_t table = vm::is_mapped(0x9B2298) ? vm::read32(0x9B2298) : 0;
        if (merge && !merge_hit) {
            static std::atomic<int> unprotected{0};
            if (table && ea < table + 0xA280 && ea + size > table && unprotected.fetch_add(1) < 200) {
                LOG_WARN("spudma", "thread 0x%x pc 0x%05x unprotected PUT to texture request table ea 0x%08x size 0x%x lsa 0x%05x", id, ctx.pc, ea, size, lsa);
            }
        }
        if (g_edge_direct >= 2 && !merge_hit && edge_loaded(ls) && in_ring(ls, ea, size) && !(table && ea < table + 0xA280 && ea + size > table)) {
            std::memcpy(vm::ptr(ea), ls + lsa, size);
            edge_direct_puts++;
            return true;
        }
        rsv::store_range(ea, ls + lsa, size);
        return true;
    }
    if (vm::is_immutable(ea, size)) return false;
    uint32_t dirty_lo = 0xFFFFFFFFu, dirty_hi = 0;
    if (rsv::copy_range_prefetch(ea, ls + lsa, size, dirty_lo, dirty_hi)) mark_ls(lsa + dirty_lo, dirty_hi - dirty_lo);
    bool snap = merge && size >= 16;
    if (snap && g_edge_direct >= 3 && edge_loaded(ls) && edge_list(ls)) {
        uint32_t table = vm::is_mapped(0x9B2298) ? vm::read32(0x9B2298) : 0;
        if (!(table && ea < table + 0xA280 && ea + size > table)) {
            snap = false;
            edge_nosnap_gets++;
        }
    }
    if (snap) remember_get(ea, size, lsa);
    else forget_ls(lsa, size);
    for (uint32_t l = lsa >> 7, last = (lsa + size - 1) >> 7; l <= last; l++) line_src[l] = ea;
    return true;
}

void SpuThread::dma(uint32_t cmd, uint32_t lsa, uint32_t ea, uint32_t size) {
    if (!size) return;
    if (g_edge_direct && dma_direct(cmd, lsa, ea, size)) return;
    if (size > 0x4000) {
        LOG_ERROR("spu", "thread 0x%x DMA size 0x%x too large", id, size);
        return;
    }
    lsa &= 0x3FFFF;
    bool put = (cmd & 0xF0) == 0x20 || (cmd & 0xF0) == 0x30 || (cmd & 0xF0) == 0xA0;
    if (put && edge::gpu::g_active && edge::gpu::on_put(ls, lsa, ea, size)) return;
    if (dma_watched(ea, size)) log_watched_dma(put ? "PUT" : "GET", cmd, ea, lsa, size, put ? ls + lsa : nullptr);
    if (ea < 0xF0000000u && (ea < 0x10000 || !vm::is_mapped(ea) || !vm::is_mapped(ea + size - 1))) {
        static std::atomic<int> reported{0};
        if (reported.fetch_add(1) < 20) {
            LOG_ERROR("spu", "thread 0x%x '%s' DMA %s cmd 0x%02x unmapped ea 0x%08x size 0x%x lsa 0x%05x pc 0x%05x", id, name.c_str(), put ? "PUT" : "GET", cmd, ea, size, lsa, ctx.pc);
        }
        if (!put) {
            std::memset(ls + lsa, 0, size);
            mark_ls(lsa, size);
        }
        return;
    }
    if (trace::on(trace::DMA)) trace::emit(trace::T_DMA, slot | (uint32_t(put) << 8) | (cmd << 16), ea, lsa, size, ctx.pc, id);
    bool handled;
    uint8_t* p = translate(ea, size, put, handled, lsa);
    if (handled || !p) return;
    if (cmd == 0x89) {
        std::memset(p, 0, size);
        return;
    }
    if (put) {
        static const uint32_t put_find = std::getenv("GOWA_SPU_PUT_FIND") ? static_cast<uint32_t>(std::strtoul(std::getenv("GOWA_SPU_PUT_FIND"), nullptr, 16)) : 0;
        static std::atomic<int> put_found{0};
        static const double put_find_at = std::getenv("GOWA_SPU_PUT_FIND_AT") ? std::atof(std::getenv("GOWA_SPU_PUT_FIND_AT")) : 0.0;
        if (put_find && put_found.load(std::memory_order_relaxed) < 12 && logging::uptime() >= put_find_at) {
            uint32_t be = __builtin_bswap32(put_find);
            for (uint32_t i = 0; i + 4 <= size; i += 4) {
                if (std::memcmp(ls + lsa + i, &be, 4) == 0) {
                    int n = put_found.fetch_add(1);
                    LOG_INFO("spudma", "thread 0x%x '%s' pc 0x%05x PUT ea 0x%08x lsa 0x%05x size 0x%x contains 0x%08x at +0x%x", id, name.c_str(), ctx.pc, ea, lsa, size, put_find, i);
                    char fn[64];
                    std::snprintf(fn, sizeof fn, "spu_put_%d_%08x.ls", n, ea);
                    if (FILE* f = std::fopen(fn, "wb")) {
                        std::fwrite(ls, 1, LS_SIZE, f);
                        std::fclose(f);
                    }
                    break;
                }
            }
        }
        static const uint32_t clr_lo = std::getenv("GOWA_SPU_PUT_CLEARS") ? static_cast<uint32_t>(std::strtoul(std::getenv("GOWA_SPU_PUT_CLEARS"), nullptr, 16)) : 0;
        static const uint32_t clr_len = std::getenv("GOWA_SPU_PUT_CLEARS_LEN") ? static_cast<uint32_t>(std::strtoul(std::getenv("GOWA_SPU_PUT_CLEARS_LEN"), nullptr, 16)) : 0x10000;
        static const uint32_t clr_mask = std::getenv("GOWA_SPU_PUT_CLEARS_MASK") ? static_cast<uint32_t>(std::strtoul(std::getenv("GOWA_SPU_PUT_CLEARS_MASK"), nullptr, 16)) : 0x8000;
        static const uint32_t clr_stride = std::getenv("GOWA_SPU_PUT_CLEARS_STRIDE") ? static_cast<uint32_t>(std::strtoul(std::getenv("GOWA_SPU_PUT_CLEARS_STRIDE"), nullptr, 16)) : 16;
        static const double clr_at = std::getenv("GOWA_SPU_PUT_CLEARS_AT") ? std::atof(std::getenv("GOWA_SPU_PUT_CLEARS_AT")) : 0.0;
        if (clr_lo && ea < clr_lo + clr_len && ea + size > clr_lo && logging::uptime() >= clr_at) {
            static std::atomic<int> clr_logged{0};
            for (uint32_t i = 0; i + 4 <= size; i += 4) {
                uint32_t a = ea + i;
                if (a < clr_lo || a >= clr_lo + clr_len || (a - clr_lo) % clr_stride) continue;
                uint32_t oldv = vm::read32(a), newv;
                std::memcpy(&newv, ls + lsa + i, 4);
                newv = __builtin_bswap32(newv);
                if ((oldv & clr_mask) && !(newv & clr_mask) && clr_logged.fetch_add(1) < 40) {
                    LOG_INFO("spudma", "thread 0x%x pc 0x%05x PUT clears mask 0x%x at 0x%08x: 0x%08x -> 0x%08x", id, ctx.pc, clr_mask, a, oldv, newv);
                }
            }
        }
        race_on_put(id, ea, size, ls + lsa, ctx.pc);
        if (ea < 0xF0000000u && put_merge_enabled() && merge_put(ea, size, lsa)) return;
        if (ea < 0xF0000000u && put_merge_enabled() && !merge_hit) {
            uint32_t table = vm::is_mapped(0x9B2298) ? vm::read32(0x9B2298) : 0;
            static std::atomic<int> unprotected{0};
            if (table && ea < table + 0xA280 && ea + size > table && unprotected.fetch_add(1) < 200) {
                LOG_WARN("spudma", "thread 0x%x pc 0x%05x unprotected PUT to texture request table ea 0x%08x size 0x%x lsa 0x%05x", id, ctx.pc, ea, size, lsa);
            }
        }
        uint32_t guest = ea;
        if (ea < 0xF0000000u) {
            rsv::store_range(guest, ls + lsa, size);
        } else {
            std::memcpy(p, ls + lsa, size);
            if (group) {
                if (SpuThread* t = group->by_slot((ea - 0xF0000000u) / 0x100000)) t->mark_ls((ea - 0xF0000000u) % 0x100000, size);
            }
        }
    } else if (ea < 0xF0000000u) {
        race_on_get(id, ea, size, ctx.pc);
        if (vm::is_immutable(ea, size)) {
            if (std::memcmp(ls + lsa, vm::ptr(ea), size) != 0) {
                std::memcpy(ls + lsa, vm::ptr(ea), size);
                mark_ls(lsa, size);
                forget_ls(lsa, size);
            }
            for (uint32_t l = lsa >> 7; l <= ((lsa + size - 1) >> 7) && l < 0x40000 / 128; l++) line_src[l] = ea;
            return;
        }
        bool snap = put_merge_enabled() && size >= 16;
        uint32_t dirty_lo = 0xFFFFFFFFu, dirty_hi = 0;
        if (rsv::copy_range_if_changed(ea, ls + lsa, size, dirty_lo, dirty_hi)) mark_ls(lsa + dirty_lo, dirty_hi - dirty_lo);
        if (snap) remember_get(ea, size, lsa);
        else forget_ls(lsa, size);
        for (uint32_t l = lsa >> 7; l <= ((lsa + size - 1) >> 7) && l < 0x40000 / 128; l++) line_src[l] = ea;
    } else {
        std::memcpy(ls + lsa, p, size);
        mark_ls(lsa, size);
    }
}

bool SpuThread::run_list(uint32_t tag, uint32_t cmd, uint32_t lsa, uint32_t list, uint32_t size) {
    uint32_t base_cmd = cmd & ~0xFu;
    uint32_t cur = lsa & 0x3FFF0;
    list &= 0x3FFF8;
    while (size >= 8) {
        uint8_t* e = ls + list;
        bool stall = (e[0] & 0x80) != 0;
        uint32_t tsize = ((uint32_t(e[2]) << 8) | e[3]) & 0x7FFF;
        uint32_t ea;
        std::memcpy(&ea, e + 4, 4);
        ea = __builtin_bswap32(ea);
        dma(base_cmd, cur | (ea & 0xF), ea, tsize);
        cur = (cur + ((tsize + 15) & ~15u)) & 0x3FFF0;
        list = (list + 8) & 0x3FFF8;
        size -= 8;
        if (stall && size >= 8) {
            std::lock_guard lk(m);
            stalled[tag] = {true, cmd, cur, list, size};
            set_events(EV_SN);
            stall_stat |= 1u << tag;
            stall_stat_count = true;
            cv.notify_all(); wake_rsv_waiter();
            return false;
        }
    }
    return true;
}

void SpuThread::process_mfc(uint32_t cmd) {
    uint32_t lsa = mfc_lsa, ea = mfc_eal, size = mfc_size, tag = mfc_tag;
    if ((cmd == 0xD0 || cmd == 0xB4 || cmd == 0xB0 || cmd == 0xB8) && (ea < 0x10000 || !vm::is_mapped(ea & ~127u))) {
        static std::atomic<int> reported{0};
        if (reported.fetch_add(1) < 6) {
            LOG_ERROR("spu", "thread 0x%x '%s' atomic cmd 0x%x on unmapped ea 0x%08x at pc 0x%05x lsa 0x%05x r0 0x%05x r1 0x%05x", id, name.c_str(), cmd, ea, ctx.pc, lsa,
                      ctx.gpr[0].u32[3], ctx.gpr[1].u32[3]);
            char fn[64];
            std::snprintf(fn, sizeof fn, "spu_fault_%08x.ls", id);
            if (FILE* f = std::fopen(fn, "wb")) {
                std::fwrite(ls, 1, LS_SIZE, f);
                std::fclose(f);
            }
        }
        std::lock_guard lk(m);
        atomic_stat = cmd == 0xD0 ? 4 : 1;
        atomic_stat_count = true;
        return;
    }
    switch (cmd) {
    case 0x20: case 0x21: case 0x22: case 0x30: case 0x31: case 0x32:
    case 0x40: case 0x41: case 0x42: case 0x89:
        dma(cmd, lsa, ea, size);
        if (cmd < 0x40) critpath::spu_stored(cp, slot, ea, size);
        else critpath::spu_label_get(cp, ea);
        break;
    case 0xA0: case 0xA1: case 0xA2:
        dma(0x20, lsa, ea, 4);
        critpath::spu_stored(cp, slot, ea, 4);
        break;
    case 0x24: case 0x25: case 0x26: case 0x34: case 0x35: case 0x36:
    case 0x44: case 0x45: case 0x46:
        run_list(tag, cmd, lsa, ea, size);
        break;
    case 0xD0: {
        uint32_t line = ea & ~127u;
        bool lost_lr = raddr && raddr != line && rsv::lost(raddr, rtime, rdata);
        uint8_t snap[128];
        uint64_t ver = rsv::snapshot(line, snap);
        if (job_stats) {
            g_getllar.fetch_add(1, std::memory_order_relaxed);
            if (raddr == line && std::memcmp(snap, rdata, 128) == 0) g_getllar_same.fetch_add(1, std::memory_order_relaxed);
        }
        if (raddr == line && ver != rtime && std::memcmp(snap, rdata, 128) == 0) lost_lr = true;
        if (lost_lr) {
            std::lock_guard lk(m);
            set_events(EV_LR);
        }
        raddr = line;
        rtime = ver;
        std::memcpy(rdata, snap, 128);
        std::memcpy(ls + (lsa & 0x3FF80), snap, 128);
        if (dma_watched(line, 128)) log_watched_dma("GETLLAR", 0xD0, line, lsa & 0x3FF80, 128, snap);
        forget_ls(lsa & 0x3FF80, 128);
        mark_ls(lsa & 0x3FF80, 128);
        atomic_stat = 4;
        atomic_stat_count = true;
        return;
    }
    case 0xB4: {
        uint32_t line = ea & ~127u;
        bool ok = raddr == line && rsv::commit_if(line, rtime, rdata, ls + (lsa & 0x3FF80), 0, 128);
        if (ok && trace::on(trace::ATOMIC)) trace::emit(trace::T_ATOMIC, slot | (0xB4u << 16), line, lsa & 0x3FF80, 128, ctx.pc, id);
        if (!ok) {
            std::lock_guard lk(m);
            set_events(EV_LR);
        }
        if (job_stats) (ok ? g_putllc_ok : g_putllc_fail).fetch_add(1, std::memory_order_relaxed);
        if (ok) critpath::spu_stored(cp, slot, line, 128);
        raddr = 0;
        atomic_stat = ok ? 0 : 1;
        atomic_stat_count = true;
        return;
    }
    case 0xB0:
    case 0xB8: {
        uint32_t line = ea & ~127u;
        rsv::store_unconditional(line, ls + (lsa & 0x3FF80));
        critpath::spu_stored(cp, slot, line, 128);
        if (trace::on(trace::ATOMIC)) trace::emit(trace::T_ATOMIC, slot | (cmd << 16), line, lsa & 0x3FF80, 128, ctx.pc, id);
        if (cmd == 0xB0) {
            atomic_stat = 2;
            atomic_stat_count = true;
        }
        return;
    }
    case 0xC0: case 0xC8: case 0xCC: case 0x80: case 0x81:
        std::atomic_thread_fence(std::memory_order_seq_cst);
        break;
    default:
        LOG_ERROR("spu", "thread 0x%x unknown MFC command 0x%x", id, cmd);
        break;
    }
}

bool SpuThread::stop_and_signal(uint32_t code) {
    switch (code) {
    case 0x001: {
        const uint64_t cp0 = critpath::g_on ? __rdtsc() : 0;
        Sleep(1);
        if (cp0) critpath::spu_waited(cp, cp0, critpath::SW_BLOCK);
        return true;
    }
    case 0x100:
        return true;
    case 0x101: {
        uint32_t status = pop_out_mbox();
        LOG_INFO("spu", "thread 0x%x group_exit(%u)", id, status);
        if (group) {
            std::lock_guard lk(group->m);
            if (!group->join_state) {
                group->join_state = 1;
                group->exit_status = status;
            }
        }
        if (group) group->stop_all();
        return false;
    }
    case 0x102: {
        exit_status = pop_out_mbox();
        exited_by_thread_exit = true;
        return false;
    }
    case 0x110:
    case 0x111: {
        uint32_t spuq_num;
        {
            std::lock_guard lk(m);
            spuq_num = out_mbox;
            out_mbox_full = false;
            if (!in_mbox.empty()) {
                in_mbox.assign(1, static_cast<uint32_t>(lv2::CELL_EBUSY));
                cv.notify_all(); wake_rsv_waiter();
                return true;
            }
        }
        uint32_t qid = 0;
        for (auto& [num, q] : spuq) {
            if (q && num == spuq_num) qid = q;
        }
        auto queue = qid ? lv2::get<lv2::EventQueue>(qid) : nullptr;
        if (!queue) {
            push_in_mbox(static_cast<uint32_t>(lv2::CELL_EINVAL));
            return true;
        }
        std::unique_lock ql(queue->m);
        while (queue->events.empty() && !queue->destroyed) {
            if (code == 0x111) {
                ql.unlock();
                push_in_mbox(static_cast<uint32_t>(lv2::CELL_EBUSY));
                return true;
            }
            if (stop_flag.load()) {
                ctx.halted = true;
                return false;
            }
            queue->cv.wait_for(ql, std::chrono::microseconds(500));
        }
        if (queue->destroyed) {
            ql.unlock();
            push_in_mbox(static_cast<uint32_t>(lv2::CELL_ECANCELED));
            return true;
        }
        lv2::Event e = queue->events.front();
        queue->events.pop_front();
        ql.unlock();
        std::lock_guard lk(m);
        in_mbox.assign({0u, static_cast<uint32_t>(e.data1), static_cast<uint32_t>(e.data2), static_cast<uint32_t>(e.data3)});
        cv.notify_all(); wake_rsv_waiter();
        return true;
    }
    default:
        LOG_ERROR("spu", "thread 0x%x unknown stop code 0x%x at 0x%05x", id, code, ctx.pc - 4);
        return false;
    }
}

void SpuThread::halt(uint32_t pc, uint32_t word) {
    LOG_ERROR("spu", "thread 0x%x '%s' halted at 0x%05x (0x%08x)", id, name.c_str(), pc, word);
}

void SpuThread::deploy() {
    for (auto& s : segments) {
        uint32_t type = s[0], lsa = s[1], size = s[2], val = s[3];
        if (type == 1) std::memcpy(ls + lsa, vm::ptr(val), size);
        else if (type == 2) {
            uint32_t be = __builtin_bswap32(val);
            for (uint32_t i = 0; i + 4 <= size; i += 4) std::memcpy(ls + lsa + i, &be, 4);
        }
    }
    mark_all_ls();
}

void SpuThread::start() {
    deploy();
    ctx = Context{};
    ctx.ls = ls;
    ctx.line_epoch = line_epoch.get();
    ctx.epoch = &ls_epoch;
    ctx.all_epoch = &ls_all_epoch;
    ctx.block_valid = region_valid.data();
    ctx.gpr[1].u32[3] = 0x3FFF0;
    for (int i = 0; i < 4; i++) ctx.gpr[3 + i].u64[1] = args[i];
    ctx.pc = image_entry & 0x3FFFC;
    ctx.interrupts_enabled = image_entry & 1;
    tag_mask = tag_stat = stall_stat = atomic_stat = 0;
    tag_stat_count = stall_stat_count = atomic_stat_count = false;
    out_mbox_full = out_intr_full = false;
    events = event_mask = 0;
    event_count = false;
    dec_value = (option & 2) ? ~static_cast<uint32_t>(timebase_now()) : 0;
    dec_start = timebase_now();
    dec_frozen = false;
    raddr = 0;
    exited_by_thread_exit = false;
    stop_flag = false;
    running = true;
    std::thread([this] { run(); }).detach();
}

namespace {

struct ChannelEvent {
    int kind;
    uint32_t ch;
    uint32_t value;
    uint32_t pc;
    uint32_t srr0;
    bool ie;
    bool halted;
    bool result;
    std::shared_ptr<std::vector<uint8_t>> ls_after;
};

struct RecordHost : Host {
    SpuThread& t;
    std::vector<ChannelEvent> events;
    explicit RecordHost(SpuThread& thread) : t(thread) {}
    void note(int kind, uint32_t ch, uint32_t value, bool result) {
        events.push_back({kind, ch, value, t.ctx.pc, t.ctx.srr0, t.ctx.interrupts_enabled, t.ctx.halted, result, nullptr});
        if (kind == 1 && (ch == 21 || ch == 26)) events.back().ls_after = std::make_shared<std::vector<uint8_t>>(t.ls, t.ls + LS_SIZE);
    }
    uint32_t read_channel(uint32_t ch) override {
        uint32_t v = t.read_channel(ch);
        note(0, ch, v, true);
        return v;
    }
    void write_channel(uint32_t ch, uint32_t value) override {
        t.write_channel(ch, value);
        note(1, ch, value, true);
    }
    uint32_t channel_count(uint32_t ch) override {
        uint32_t v = t.channel_count(ch);
        note(2, ch, v, true);
        return v;
    }
    bool stop_and_signal(uint32_t code) override {
        bool r = t.stop_and_signal(code);
        note(3, code, 0, r);
        return r;
    }
    void halt(uint32_t pc, uint32_t word) override {
        t.halt(pc, word);
        note(4, pc, word, true);
    }
};

struct ReplayHost : Host {
    Context& c;
    const std::vector<ChannelEvent>& events;
    size_t pos = 0;
    std::string error;
    ReplayHost(Context& ctx, const std::vector<ChannelEvent>& ev) : c(ctx), events(ev) {}
    const ChannelEvent* next(int kind, uint32_t ch, uint32_t value, bool check_value) {
        if (pos >= events.size()) {
            if (error.empty()) error = "extra channel op kind " + std::to_string(kind) + " ch " + std::to_string(ch);
            return nullptr;
        }
        const ChannelEvent& e = events[pos++];
        if (e.kind != kind || e.ch != ch || (check_value && e.value != value)) {
            if (error.empty()) {
                char b[160];
                std::snprintf(b, sizeof b, "channel op %zu: aot kind %d ch %u val 0x%x, int kind %d ch %u val 0x%x", pos - 1, kind, ch, value, e.kind, e.ch, e.value);
                error = b;
            }
        }
        c.pc = e.pc;
        c.srr0 = e.srr0;
        c.interrupts_enabled = e.ie;
        c.halted = e.halted;
        if (e.ls_after) std::memcpy(c.ls, e.ls_after->data(), LS_SIZE);
        return &e;
    }
    uint32_t read_channel(uint32_t ch) override {
        const ChannelEvent* e = next(0, ch, 0, false);
        return e ? e->value : 0;
    }
    void write_channel(uint32_t ch, uint32_t value) override { next(1, ch, value, true); }
    uint32_t channel_count(uint32_t ch) override {
        const ChannelEvent* e = next(2, ch, 0, false);
        return e ? e->value : 0;
    }
    bool stop_and_signal(uint32_t code) override {
        const ChannelEvent* e = next(3, code, 0, false);
        return e ? e->result : false;
    }
    void halt(uint32_t pc, uint32_t word) override { next(4, pc, word, true); }
};

struct CaptureHost : Host {
    SpuThread& t;
    std::vector<uint8_t> out;
    uint32_t events = 0;
    explicit CaptureHost(SpuThread& thread) : t(thread) {}
    template <class T>
    void put(const T& v) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
        out.insert(out.end(), p, p + sizeof v);
    }
    void begin() {}
    void note_irq(uint32_t at) {
        begin();
        note(5, 0, at, true);
    }
    void note(int32_t kind, uint32_t ch, uint32_t value, bool result) {
        events++;
        put(kind);
        put(ch);
        put(value);
        put(t.ctx.pc);
        put(t.ctx.srr0);
        uint8_t flags[4] = {uint8_t(t.ctx.interrupts_enabled), uint8_t(t.ctx.halted), uint8_t(result), 0};
        put(flags);
        size_t count_at = out.size();
        put(uint32_t(0));
        std::vector<uint32_t> written;
        while (t.cap_lock.test_and_set(std::memory_order_acquire)) {
        }
        written.swap(t.cap_lines);
        t.cap_lock.clear(std::memory_order_release);
        std::sort(written.begin(), written.end());
        written.erase(std::unique(written.begin(), written.end()), written.end());
        uint32_t lines = 0;
        for (uint32_t l : written) {
            put(l << 7);
            out.insert(out.end(), t.ls + (l << 7), t.ls + (l << 7) + 128);
            lines++;
        }
        std::memcpy(out.data() + count_at, &lines, 4);
    }
    uint32_t read_channel(uint32_t ch) override {
        begin();
        uint32_t v = t.read_channel(ch);
        note(0, ch, v, true);
        return v;
    }
    void write_channel(uint32_t ch, uint32_t value) override {
        begin();
        t.write_channel(ch, value);
        note(1, ch, value, true);
    }
    uint32_t channel_count(uint32_t ch) override {
        begin();
        uint32_t v = t.channel_count(ch);
        note(2, ch, v, true);
        return v;
    }
    bool stop_and_signal(uint32_t code) override {
        begin();
        bool r = t.stop_and_signal(code);
        note(3, code, 0, r);
        return r;
    }
    void halt(uint32_t pc, uint32_t word) override {
        begin();
        t.halt(pc, word);
        note(4, pc, word, true);
    }
};

std::atomic<uint32_t> g_cap_gen{0};
std::mutex g_cap_dir_lock;
std::string g_cap_dir;

void write_capture(SpuThread& t, const Context& start, const std::vector<uint8_t>& ls0, CaptureHost& cap, uint32_t insns, const std::string& dir) {
    CreateDirectoryA(dir.c_str(), nullptr);
    std::string path = dir + "/cap_" + std::to_string(t.slot) + ".bin";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    uint32_t hdr[4] = {0x43555053u, 1u, t.slot, insns};
    std::fwrite(hdr, 4, 4, f);
    uint32_t st[3] = {start.pc, start.srr0, uint32_t(start.interrupts_enabled)};
    std::fwrite(st, 4, 3, f);
    std::fwrite(start.gpr, 16, 128, f);
    std::fwrite(ls0.data(), 1, LS_SIZE, f);
    std::fwrite(&cap.events, 4, 1, f);
    std::fwrite(cap.out.data(), 1, cap.out.size(), f);
    uint32_t fin[3] = {t.ctx.pc, t.ctx.srr0, uint32_t(t.ctx.interrupts_enabled)};
    std::fwrite(fin, 4, 3, f);
    std::fwrite(t.ctx.gpr, 16, 128, f);
    std::fwrite(t.ls, 1, LS_SIZE, f);
    std::fclose(f);
    LOG_INFO("spucap", "thread %u captured %u insns, %u channel events, %zu bytes to %s", t.slot, insns, cap.events, cap.out.size(), dir.c_str());
}

}

void SpuThread::run() {
    wchar_t w[64];
    swprintf(w, 64, L"SPU %u %hs", slot, name.c_str());
    SetThreadDescription(GetCurrentThread(), w);
    if (std::getenv("GOWA_SPU_LOW_PRIO")) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    if (const char* aff = std::getenv("GOWA_SPU_AFFINITY")) {
        int stride = std::atoi(aff);
        if (stride > 0) SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << ((slot * stride) % 12));
    }
    placement::apply("spu" + std::to_string(slot));
    _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x6000u | 0x8040u);
    LOG_INFO("spu", "thread 0x%x '%s' start pc 0x%05x args 0x%llx 0x%llx", id, name.c_str(), ctx.pc,
             static_cast<unsigned long long>(args[0]), static_cast<unsigned long long>(args[1]));
    if (!native) {
        HANDLE hh = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &hh, 0, FALSE, DUPLICATE_SAME_ACCESS);
        native = hh;
    }
    hle::install_builtin();
    register_thread(this);
    if (const char* e = std::getenv("GOWA_PROFILE_SPU")) {
        if (std::string(e) == "all" || static_cast<uint32_t>(std::atoi(e)) == slot) {
            HANDLE h = nullptr;
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
            static std::string labels[8];
            std::string& label = labels[slot & 7];
            label = "spu " + std::to_string(slot);
            crash::profile_thread(h, label.c_str());
        }
    }
    uint32_t n = 0;
    static const std::vector<uint32_t> bps = [] {
        std::vector<uint32_t> v;
        if (const char* e = std::getenv("GOWA_SPU_BP")) {
            std::string s = e;
            size_t pos = 0;
            while (pos < s.size()) {
                size_t end = s.find(',', pos);
                if (end == std::string::npos) end = s.size();
                v.push_back(static_cast<uint32_t>(std::strtoul(s.substr(pos, end - pos).c_str(), nullptr, 16)));
                pos = end + 1;
            }
        }
        return v;
    }();
    const bool aot_on = aot::active();
    const bool verify = std::getenv("GOWA_SPU_AOT_VERIFY") != nullptr;
    bool halted_in_verify = false;
    const bool pure_only = std::getenv("GOWA_SPU_AOT_PURE_ONLY") != nullptr;
    std::vector<uint8_t> verify_ls(verify ? LS_SIZE : 0);
    std::unordered_map<const CompiledBlock*, int> verify_count;
    auto verify_block = [&](const CompiledBlock* b) -> bool {
        int& cnt = verify_count[b];
        if (cnt >= 30) return false;
        cnt++;
        Context copy = ctx;
        std::memcpy(verify_ls.data(), ls, LS_SIZE);
        copy.ls = verify_ls.data();
        uint32_t start = ctx.pc;
        RecordHost recorder(*this);
        bool int_ok = true;
        for (uint32_t k = 0; k < b->words; k++) {
            int_ok = step(ctx, recorder);
            if (!int_ok) break;
            if (k + 1 < b->words && ctx.pc != ((start + 4 * (k + 1)) & PC_MASK)) break;
        }
        ReplayHost replay(copy, recorder.events);
        bool aot_ok = b->fn(copy, replay);
        if (replay.error.empty() && replay.pos != recorder.events.size()) replay.error = "aot consumed " + std::to_string(replay.pos) + " of " + std::to_string(recorder.events.size()) + " channel ops";
        bool bad = !replay.error.empty() || aot_ok != int_ok || copy.pc != ctx.pc || copy.srr0 != ctx.srr0 || copy.interrupts_enabled != ctx.interrupts_enabled ||
                   std::memcmp(copy.gpr, ctx.gpr, sizeof ctx.gpr) != 0 || std::memcmp(verify_ls.data(), ls, LS_SIZE) != 0;
        if (bad) {
            std::string diff;
            for (int r = 0; r < 128; r++) {
                if (std::memcmp(&copy.gpr[r], &ctx.gpr[r], 16) == 0) continue;
                char t[96];
                std::snprintf(t, sizeof t, " r%d aot=%08x%08x%08x%08x int=%08x%08x%08x%08x", r, copy.gpr[r].u32[3], copy.gpr[r].u32[2], copy.gpr[r].u32[1], copy.gpr[r].u32[0], ctx.gpr[r].u32[3], ctx.gpr[r].u32[2],
                              ctx.gpr[r].u32[1], ctx.gpr[r].u32[0]);
                diff += t;
            }
            LOG_ERROR("spuaot", "MISMATCH block lsa 0x%05x words %u pure %d: ok %d/%d pc aot 0x%05x int 0x%05x ie %d/%d ls %s %s%s", start, b->words, b->pure ? 1 : 0, aot_ok, int_ok, copy.pc, ctx.pc,
                      copy.interrupts_enabled, ctx.interrupts_enabled, std::memcmp(verify_ls.data(), ls, LS_SIZE) ? "differs" : "same", replay.error.c_str(), diff.c_str());
        }
        if (!int_ok) halted_in_verify = true;
        return true;
    };
    struct alignas(32) Slot {
        const CompiledBlock* b;
        BlockFn fn;
        BlockFn region;
        uint32_t words;
        uint32_t valid;
    };
    std::vector<Slot> block_cache(aot_on ? LS_SIZE / 4 : 0, Slot{});
    struct HleSlot {
        const hle::Function* f;
        uint32_t valid;
    };
    static const bool hle_cache_on = [] {
        const char* e = std::getenv("GOWA_SPU_HLE_CACHE");
        return !e || e[0] != '0';
    }();
    std::vector<HleSlot> hle_cache(hle_cache_on ? LS_SIZE / 4 : 0, HleSlot{});
    auto hle_find = [&](uint32_t pc) -> const hle::Function* {
        if (!hle_cache_on) return hle::lookup(ls, pc);
        HleSlot& hs = hle_cache[pc >> 2];
        uint32_t w = hle::max_sig_words(pc);
        if (hs.valid && ls_fresh(pc, w, hs.valid)) return hs.f;
        uint32_t e = ls_epoch.load(std::memory_order_acquire);
        const hle::Function* f = hle::lookup(ls, pc);
        hs.f = f;
        hs.valid = w < 255 ? e : 0;
        return f;
    };
    static const bool no_fresh = std::getenv("GOWA_SPU_NO_FRESH") != nullptr;
    const bool use_regions = !verify && !std::getenv("GOWA_SPU_NO_REGIONS");
    uint32_t irq_countdown = 256;
    uint64_t cyc_mark = 0;
    Host* host = this;
    static const double cap_at = std::getenv("GOWA_SPU_CAPTURE_AT") ? std::atof(std::getenv("GOWA_SPU_CAPTURE_AT")) : 0.0;
    static const uint32_t cap_insns = std::getenv("GOWA_SPU_CAPTURE_INSNS") ? static_cast<uint32_t>(std::strtoul(std::getenv("GOWA_SPU_CAPTURE_INSNS"), nullptr, 0)) : 20000000u;
    std::unique_ptr<CaptureHost> cap;
    Context cap_start;
    std::vector<uint8_t> cap_ls;
    uint32_t cap_n = 0;
    bool cap_done = false;
    uint32_t cap_gen_seen = g_cap_gen.load();
    std::string cap_dir;
    uint64_t loc_aot = 0, loc_calls = 0, loc_region = 0;
    static const uint64_t slow_cycles = std::getenv("GOWA_SPU_SLOW") ? std::strtoull(std::getenv("GOWA_SPU_SLOW"), nullptr, 0) : 0;
    std::unordered_map<const void*, BlockCyc> local_cyc;
    uint32_t local_cyc_n = 0;
    uint32_t cyc_insn_mark = 0;
    const bool rec = aot::recording();
    bool block_start = true;
    uint32_t block_len = 0;
    static const int job_log = std::getenv("GOWA_SPU_JOBS") ? std::atoi(std::getenv("GOWA_SPU_JOBS")) : 0;
    const bool cp_on = critpath::g_on;
    if (cp_on) critpath::spu_init_sig();
    const uint32_t* cp_src = line_src.get();
    for (;;) {
        if (block_start) {
            block_start = false;
            block_len = 0;
            if (cp_on) critpath::spu_block(cp, slot, ls, cp_src, ctx.pc);
            if (job_log) {
                uint32_t src = line_src[ctx.pc >> 7];
                if (src != cur_src) {
                    uint32_t prev = cur_src;
                    uint64_t delta = uint32_t(n - uint32_t(src_insn_mark));
                    src_insn_mark = n;
                    cur_src = src;
                    uint32_t wid;
                    std::memcpy(&wid, ls + 0x1dc, 4);
                    note_job(id, __builtin_bswap32(wid), src, ctx.pc, job_log, prev, delta);
                }
            }
            if (!cap && hle::any_at(ctx.pc)) {
                if (const hle::Function* f = hle_find(ctx.pc)) {
                    if (f->host_handler) {
                        ctx.executed = 0;
                        uint32_t pc_before = ctx.pc;
                        if (f->host_handler(ctx, *host)) {
                            uint32_t ran = ctx.executed;
                            n += ran;
                            if (ctx.halted) break;
                            if (uint32_t crossed = (n >> 10) - ((n - ran) >> 10)) {
                                insns.fetch_add(uint64_t(crossed) << 10, std::memory_order_relaxed);
                                if (stop_flag.load(std::memory_order_relaxed)) break;
                            }
                            if (ctx.irq_check || ctx.interrupts_enabled) {
                                ctx.irq_check = false;
                                uint32_t before_irq = ctx.pc;
                                check_interrupt();
                                if (cap && ctx.pc != before_irq) cap->note_irq(n - cap_n);
                            }
                            block_start = true;
                            continue;
                        }
                        ctx.pc = pc_before;
                    } else if (f->handler(ctx)) {
                        ctx.pc = ctx.gpr[0].u32[3] & PC_MASK;
                        block_start = true;
                        continue;
                    }
                }
            }
            if (aot_on) {
                Slot& slot = block_cache[ctx.pc >> 2];
                const CompiledBlock* b = slot.b;
                if (!b || no_fresh || !ls_fresh(ctx.pc, slot.words, slot.valid)) {
                    uint32_t e = ls_epoch.load(std::memory_order_acquire);
                    if (!b || std::memcmp(ls + ctx.pc, b->code, b->words * 4) != 0) {
                        b = aot::find(ls, ctx.pc);
                        slot.b = b;
                        slot.fn = b ? b->fn : nullptr;
                        slot.region = b && use_regions ? b->region : nullptr;
                        slot.words = b ? b->words : 0;
                    }
                    slot.valid = b ? e : 0;
                }
                if (b && verify && verify_block(b)) {
                    n += b->words;
                    block_start = true;
                    if (halted_in_verify) break;
                    continue;
                }
                if (b && pure_only && !b->pure) b = nullptr;
                if (b) {
                    bool ok;
                    uint32_t ran = slot.words;
                    uint64_t t0 = job_log >= 3 ? __rdtsc() : 0;
                    if (slot.region) {
                        ctx.executed = 0;
                        ok = slot.region(ctx, *host);
                        if (ctx.executed) {
                            ran = ctx.executed;
                            if (job_log) loc_region += ran;
                        }
                        else ok = slot.fn(ctx, *host);
                    } else {
                        ok = slot.fn(ctx, *host);
                    }
                    n += ran;
                    if (job_log) {
                        loc_aot += ran;
                        loc_calls++;
                        if (job_log >= 3) {
                            uint64_t dt = __rdtsc() - t0;
                            g_gen_cycles.fetch_add(dt, std::memory_order_relaxed);
                            local_cyc[b].cycles += dt;
                            local_cyc[b].insns += ran;
                            local_cyc[b].calls++;
                            if (++local_cyc_n >= 20000) {
                                std::lock_guard lk(g_job_lock);
                                for (auto& [k, v] : local_cyc) {
                                    BlockCyc& g = g_block_cyc[k];
                                    g.cycles += v.cycles;
                                    g.insns += v.insns;
                                    g.calls += v.calls;
                                }
                                local_cyc.clear();
                                local_cyc_n = 0;
                            }
                        }
                    }
                    if (!ok) break;
                    block_start = true;
                    uint32_t pc_before_irq = ctx.pc;
                    if (ctx.irq_check) {
                        ctx.irq_check = false;
                        check_interrupt();
                    } else if (ctx.interrupts_enabled) {
                        if (irq_countdown <= ran) {
                            irq_countdown = 256;
                            check_interrupt();
                        } else {
                            irq_countdown -= ran;
                        }
                    }
                    if (cap && ctx.pc != pc_before_irq) cap->note_irq(n - cap_n);
                    if ((n & ~1023u) != ((n - ran) & ~1023u)) {
                        insns.fetch_add(1024, std::memory_order_relaxed);
                        if (slow_cycles) {
                            uint64_t until = __rdtsc() + slow_cycles;
                            while (__rdtsc() < until) _mm_pause();
                        }
                        if (job_log && (n & ~0xFFFFu) != ((n - ran) & ~0xFFFFu)) {
                            ULONG64 cyc = 0;
                            QueryThreadCycleTime(GetCurrentThread(), &cyc);
                            if (cyc_mark) {
                                g_cyc_cycles.fetch_add(cyc - cyc_mark, std::memory_order_relaxed);
                                g_cyc_insns.fetch_add(uint32_t(n - cyc_insn_mark), std::memory_order_relaxed);
                            }
                            g_aot_insns.fetch_add(loc_aot, std::memory_order_relaxed);
                            g_block_calls.fetch_add(loc_calls, std::memory_order_relaxed);
                            g_region_insns.fetch_add(loc_region, std::memory_order_relaxed);
                            loc_aot = loc_calls = loc_region = 0;
                            cyc_mark = cyc;
                            cyc_insn_mark = n;
                        }
                        pc_samples[pc_sample_pos.fetch_add(1, std::memory_order_relaxed) & 255] = ctx.pc;
                        if (stop_flag.load(std::memory_order_relaxed)) break;
                        bool cap_start_now = false;
                        if (!cap) {
                            uint32_t g = g_cap_gen.load(std::memory_order_relaxed);
                            if (g != cap_gen_seen) {
                                cap_gen_seen = g;
                                std::lock_guard lk(g_cap_dir_lock);
                                cap_dir = g_cap_dir;
                                cap_start_now = true;
                            } else if (cap_at > 0.0 && !cap_done && logging::uptime() >= cap_at) {
                                cap_dir = "spucap";
                                cap_done = true;
                                cap_start_now = true;
                            }
                        }
                        if (cap_start_now || cap) {
                            if (!cap) {
                                cap = std::make_unique<CaptureHost>(*this);
                                cap_lines.clear();
                                cap_on.store(true);
                                cap_start = ctx;
                                cap_ls.assign(ls, ls + LS_SIZE);
                                cap_n = n;
                                host = cap.get();
                            } else if (cap && n - cap_n >= cap_insns) {
                                cap_on.store(false);
                                write_capture(*this, cap_start, cap_ls, *cap, n - cap_n, cap_dir);
                                cap.reset();
                                host = this;
                            }
                        }
                    }
                    continue;
                }
            }
            if (rec) aot::record(ls, ctx.pc);
            if (job_log) {
                std::lock_guard lk(g_job_lock);
                g_interp_pcs[ctx.pc]++;
            }
        }
        uint32_t word;
        std::memcpy(&word, ls + ctx.pc, 4);
        word = __builtin_bswap32(word);
        if (job_log) g_interp_insns.fetch_add(1, std::memory_order_relaxed);
        if (!step(ctx, *host)) break;
        if (aot::is_terminator(word) || ++block_len >= aot::MAX_BLOCK_WORDS) block_start = true;
        if (!bps.empty()) {
            for (uint32_t bp : bps) {
                if (ctx.pc == bp) {
                    uint32_t wid;
                    std::memcpy(&wid, ls + 0x1dc, 4);
                    LOG_INFO("spubp", "thread 0x%x pc 0x%05x wid %u r3 %08x r4 %08x r5 %08x lr(r0) %05x", id, ctx.pc, __builtin_bswap32(wid), ctx.gpr[3].u32[3], ctx.gpr[4].u32[3],
                             ctx.gpr[5].u32[3], ctx.gpr[0].u32[3]);
                    break;
                }
            }
        }
        if (ctx.irq_check) {
            ctx.irq_check = false;
            check_interrupt();
        } else if (ctx.interrupts_enabled && (n & 63) == 0) {
            check_interrupt();
        }
        if ((++n & 1023) == 0) {
            insns.fetch_add(1024, std::memory_order_relaxed);
            pc_samples[pc_sample_pos.fetch_add(1, std::memory_order_relaxed) & 255] = ctx.pc;
            if (stop_flag.load(std::memory_order_relaxed)) break;
        }
    }
    if (rec) aot::flush();
    LOG_INFO("spu", "thread 0x%x '%s' stopped at 0x%05x", id, name.c_str(), ctx.pc);
    {
        std::lock_guard lk(m);
        running = false;
        in_mbox.clear();
    }
    if (group) group->thread_stopped(*this);
}

namespace {
std::mutex g_reg_lock;
std::vector<SpuThread*> g_reg;
}

void occupancy_sampler(double period_s, int periods, std::string out_path) {
    FILE* out = out_path.empty() ? nullptr : std::fopen(out_path.c_str(), "w");
    auto emit = [&](const char* tag, const std::string& text) {
        LOG_INFO("spuocc", "%s:%s", tag, text.c_str());
        if (out) std::fprintf(out, "%s:%s\n", tag, text.c_str());
    };
    std::map<std::pair<uint32_t, uint32_t>, uint64_t> by_job;
    std::map<uint32_t, uint64_t> kpcs;
    uint64_t conc[8] = {};
    uint64_t samples = 0;
    auto last = std::chrono::steady_clock::now();
    std::map<SpuThread*, std::pair<uint64_t, uint64_t>> prev_cpu;
    uint64_t prev_tsc = __rdtsc();
    {
        std::lock_guard lk(g_reg_lock);
        for (SpuThread* t : g_reg) {
            ULONG64 cyc = 0;
            if (t->native) QueryThreadCycleTime(static_cast<HANDLE>(t->native), &cyc);
            prev_cpu[t] = {cyc, t->insns.load()};
        }
    }
    for (;;) {
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        int busy = 0;
        {
            std::lock_guard lk(g_reg_lock);
            for (SpuThread* t : g_reg) {
                if (!t->running) continue;
                uint32_t pc = std::atomic_ref<uint32_t>(t->ctx.pc).load(std::memory_order_relaxed);
                uint32_t wid;
                std::memcpy(&wid, t->ls + 0x1dc, 4);
                wid = __builtin_bswap32(wid);
                uint32_t src = t->line_src[(pc & 0x3FFFF) >> 7];
                bool job = pc >= 0x4000;
                if (job) busy++;
                uint32_t w = t->waiting.load(std::memory_order_relaxed);
                if (w) by_job[{0xFFFFFFF0u + w, 0}]++;
                else by_job[{job ? wid : 0xFFFFFFFFu, job ? src : 0}]++;
                if (!w && !job) kpcs[pc & ~0xFu]++;
            }
        }
        conc[(std::min)(busy, 7)]++;
        samples++;
        auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::duration<double>(period_s)) continue;
        double secs = std::chrono::duration<double>(now - last).count();
        last = now;
        std::vector<std::pair<std::pair<uint32_t, uint32_t>, uint64_t>> v(by_job.begin(), by_job.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        std::string line;
        for (size_t i = 0; i < v.size() && i < 40; i++) {
            char b[64];
            if (v[i].first.first == 0xFFFFFFFFu) std::snprintf(b, sizeof b, " [kernel %.2f]", double(v[i].second) / samples);
            else if (v[i].first.first > 0xFFFFFFF0u) std::snprintf(b, sizeof b, " [hostwait%u %.2f]", v[i].first.first - 0xFFFFFFF0u, double(v[i].second) / samples);
            else std::snprintf(b, sizeof b, " [wid %u %08x %.2f]", v[i].first.first, v[i].first.second, double(v[i].second) / samples);
            line += b;
        }
        emit("avg SPUs per job", line);
        {
            std::vector<std::pair<uint32_t, uint64_t>> kv(kpcs.begin(), kpcs.end());
            std::sort(kv.begin(), kv.end(), [](auto& a, auto& b) { return a.second > b.second; });
            std::string k;
            for (size_t i = 0; i < kv.size() && i < 16; i++) {
                char b[48];
                std::snprintf(b, sizeof b, " %05x:%.3f", kv[i].first, double(kv[i].second) / samples);
                k += b;
            }
            emit("kernel spin pcs", k);
            emit("lr waits", rsv::wait_stats_take());
            kpcs.clear();
        }
        std::string c;
        for (int i = 0; i < 8; i++) {
            char b[32];
            std::snprintf(b, sizeof b, " %d:%.0f%%", i, 100.0 * conc[i] / samples);
            c += b;
        }
        emit("SPUs running job code", c);
        {
            uint64_t tsc = __rdtsc();
            std::string u;
            std::lock_guard lk(g_reg_lock);
            for (SpuThread* t : g_reg) {
                ULONG64 cyc = 0;
                if (t->native) QueryThreadCycleTime(static_cast<HANDLE>(t->native), &cyc);
                uint64_t ins = t->insns.load();
                auto& pv = prev_cpu[t];
                char b[96];
                if (pv.first && cyc >= pv.first && ins >= pv.second) std::snprintf(b, sizeof b, " [%u cpu %.0f%% %.0fM insn/s]", t->slot, 100.0 * double(cyc - pv.first) / double(tsc - prev_tsc), double(ins - pv.second) / secs / 1e6);
                else b[0] = 0;
                u += b;
                pv = {cyc, ins};
            }
            prev_tsc = tsc;
            if (!u.empty()) emit("thread cpu", u);
        }
        by_job.clear();
        std::memset(conc, 0, sizeof conc);
        samples = 0;
        if (periods > 0 && --periods == 0) break;
    }
    if (out) std::fclose(out);
}

void request_capture(const std::string& dir) {
    {
        std::lock_guard lk(g_cap_dir_lock);
        g_cap_dir = dir;
    }
    g_cap_gen.fetch_add(1);
}

void lowfps_tick(uint64_t flips) {
    struct Cfg {
        double fps = 0, secs = 6, cooldown = 90, occ_secs = 5;
        int max = 4;
        std::string dir = "spucap_lowfps";
    };
    static const Cfg cfg = [] {
        Cfg c;
        if (const char* e = std::getenv("GOWA_SPU_CAPTURE_LOWFPS")) std::sscanf(e, "%lf:%lf:%lf:%d", &c.fps, &c.secs, &c.cooldown, &c.max);
        if (const char* d = std::getenv("GOWA_SPU_CAPTURE_DIR")) c.dir = d;
        return c;
    }();
    if (cfg.fps <= 0) return;
    static double last_t = -1, low_since = -1, last_trigger = -1e9;
    static uint64_t last_flips = 0;
    static int taken = 0;
    double now = logging::uptime();
    if (last_t < 0) {
        last_t = now;
        last_flips = flips;
        return;
    }
    double dt = now - last_t;
    if (dt < 0.9) return;
    double fps = double(flips - last_flips) / dt;
    last_t = now;
    last_flips = flips;
    if (fps < 2.0 || fps >= cfg.fps) {
        low_since = -1;
        return;
    }
    if (low_since < 0) low_since = now - dt;
    if (now - low_since < cfg.secs || now - last_trigger < cfg.cooldown || taken >= cfg.max) return;
    last_trigger = now;
    low_since = -1;
    taken++;
    CreateDirectoryA(cfg.dir.c_str(), nullptr);
    char name[96];
    std::snprintf(name, sizeof name, "/low%d_t%.0f_fps%.1f", taken, now, fps);
    std::string dir = cfg.dir + name;
    CreateDirectoryA(dir.c_str(), nullptr);
    LOG_INFO("spucap", "low fps %.1f for %.0f s: capturing SPU state and occupancy to %s", fps, cfg.secs, dir.c_str());
    request_capture(dir);
    std::thread(occupancy_sampler, cfg.occ_secs, 1, dir + "/occ.txt").detach();
}

void register_thread(SpuThread* t) {
    std::lock_guard lk(g_reg_lock);
    if (std::find(g_reg.begin(), g_reg.end(), t) == g_reg.end()) g_reg.push_back(t);
    static std::once_flag once;
    if (std::getenv("GOWA_SPU_OCC")) std::call_once(once, [] { std::thread(occupancy_sampler, 10.0, 0, std::string()).detach(); });
}

std::string brief() {
    std::string out;
    std::lock_guard lk(g_reg_lock);
    for (SpuThread* t : g_reg) {
        char b[96];
        uint32_t wid;
        std::memcpy(&wid, t->ls + 0x1dc, 4);
        std::snprintf(b, sizeof b, " [%x pc %05x wid %u ra %08x]", t->id >> 24, t->ctx.pc, __builtin_bswap32(wid), t->raddr);
        out += b;
    }
    return out;
}

void status_dump() {
    if (const char* e = std::getenv("GOWA_DUMP")) {
        std::string list = e;
        size_t pos = 0;
        while (pos < list.size()) {
            size_t end = list.find(',', pos);
            if (end == std::string::npos) end = list.size();
            uint32_t a = static_cast<uint32_t>(std::strtoul(list.substr(pos, end - pos).c_str(), nullptr, 16));
            for (uint32_t off = 0; off < 0x100; off += 32) {
                std::string hex;
                char b[12];
                for (uint32_t i = 0; i < 32; i += 4) {
                    std::snprintf(b, sizeof b, "%08x ", vm::read32(a + off + i));
                    hex += b;
                }
                LOG_INFO("status", "mem %08x: %s", a + off, hex.c_str());
            }
            pos = end + 1;
        }
    }
    std::lock_guard lk(g_reg_lock);
    for (SpuThread* t : g_reg) {
        LOG_INFO("status", "spu 0x%x '%s' pc 0x%05x ie %d srr0 0x%05x insns %llu mask 0x%x events 0x%x raddr 0x%08x running %d", t->id, t->name.c_str(), t->ctx.pc, t->ctx.interrupts_enabled ? 1 : 0, t->ctx.srr0,
                 static_cast<unsigned long long>(t->insns.load()), t->event_mask, t->events, t->raddr, t->running ? 1 : 0);
        std::map<uint32_t, int> hist;
        for (uint32_t pc : t->pc_samples) hist[pc & ~0x3Fu]++;
        std::vector<std::pair<int, uint32_t>> top;
        for (auto& [pc, n] : hist) top.push_back({n, pc});
        std::sort(top.rbegin(), top.rend());
        std::string line;
        for (size_t i = 0; i < top.size() && i < 6; i++) line += " " + std::to_string(top[i].first) + "@" + std::to_string(top[i].second);
        LOG_INFO("status", "   hot pcs (count@addr dec):%s", line.c_str());
        char fn[64];
        std::snprintf(fn, sizeof fn, "spu_%08x.ls", t->id);
        if (FILE* f = std::fopen(fn, "wb")) {
            std::fwrite(t->ls, 1, LS_SIZE, f);
            std::fclose(f);
        }
    }
}

SpuThread* Group::by_slot(uint32_t slot) {
    for (auto& t : threads) {
        if (t && t->slot == slot) return t.get();
    }
    return nullptr;
}

void Group::thread_stopped(SpuThread& t) {
    std::lock_guard lk(m);
    if (running) running--;
    if (!running) {
        state = 1;
        if (!join_state) {
            join_state = 2;
            exit_status = 0;
        }
        cv.notify_all();
        LOG_INFO("spu", "group 0x%x '%s' stopped (cause %u status %u)", id, name.c_str(), join_state, exit_status);
    }
}

void Group::stop_all() {
    for (auto& t : threads) {
        if (!t) continue;
        t->stop_flag = true;
        t->cv.notify_all();
        rsv::park_kick(t->park_slot);
    }
}

}
