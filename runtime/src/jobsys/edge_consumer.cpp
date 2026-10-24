#include "edge_consumer.h"
#include "edge_exec.h"

#include "../spu/hle.h"
#include "../spu/thread.h"

#include "ppu/context.h"
#include "runtime/critpath.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/reservation.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <immintrin.h>

namespace jsedge {

namespace {

constexpr uint32_t kSpurs = 0x3001ad00;
constexpr uint32_t kVspus = 4;
constexpr uint32_t kFirstWid = 19;
constexpr uint32_t kLists = 3;

struct Config {
    bool on = false;
    int threads = 3;
    std::vector<int> cpus;
    int prio = 0;
    bool prio_set = false;
    bool help = true;
    uint32_t verify = 0;
    bool reference = false;
    uint32_t vspus = kVspus;
};

std::vector<int> parse_list(const char* e) {
    std::vector<int> v;
    if (!e) return v;
    std::string s = e;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        if (end > pos) v.push_back(static_cast<int>(std::strtol(s.substr(pos, end - pos).c_str(), nullptr, 0)));
        pos = end + 1;
    }
    return v;
}

const Config& cfg() {
    static const Config c = [] {
        Config c;
        const char* e = std::getenv("GOWA_JOBSYS_EDGE");
        c.on = e && e[0] && e[0] != '0';
        if (const char* t = std::getenv("GOWA_JOBSYS_EDGE_THREADS")) c.threads = std::clamp(std::atoi(t), 0, 8);
        c.cpus = parse_list(std::getenv("GOWA_JOBSYS_EDGE_CPUS"));
        if (const char* p = std::getenv("GOWA_JOBSYS_EDGE_PRIO")) {
            c.prio = std::atoi(p);
            c.prio_set = true;
        }
        if (const char* h = std::getenv("GOWA_JOBSYS_EDGE_HELP")) c.help = h[0] != '0';
        if (const char* v = std::getenv("GOWA_JOBSYS_EDGE_VERIFY")) c.verify = static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
        if (const char* r = std::getenv("GOWA_JOBSYS_EDGE_REF")) c.reference = r[0] == '1';
        if (const char* n = std::getenv("GOWA_JOBSYS_EDGE_VSPUS")) c.vspus = static_cast<uint32_t>(std::clamp(std::atoi(n), 1, 4));
        return c;
    }();
    return c;
}

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
void wbe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}
uint32_t be32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

enum State : int { S_SPU = 0, S_PENDING = 1, S_OWNED = 2, S_HANDBACK = 3, S_DISABLED = 4 };

struct List {
    uint32_t wid = 0;
    uint32_t info = 0;
    uint32_t hdr = 0;
    std::atomic<int> state{S_SPU};
    uint8_t prio_saved[8] = {};
    std::atomic<int> held{0};
    int stable = 0;
};

List g_lists[kLists];

struct VSpu {
    std::atomic<bool> busy{false};
    std::unique_ptr<Executor> x;
};

VSpu g_vspu[kVspus];

struct Stats {
    std::atomic<uint64_t> edge{0}, ab{0}, cr{0}, nops{0}, barriers{0}, blocked{0}, rc_zero{0}, helped{0}, failed{0}, unsupported{0};
    std::atomic<uint64_t> tsc{0}, cpu{0}, ab_wait{0};
    std::atomic<uint64_t> gets{0}, get_bytes{0}, puts{0}, put_bytes{0}, slow{0};
    std::atomic<uint64_t> lv_runs{0}, lv_bad{0}, xv_runs{0}, xv_bad{0}, xv_skip{0}, xv_bytes{0};
};
Stats g_st;

std::atomic<bool> g_started{false};

uint64_t thread_cycles() {
    ULONG64 c = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c);
    return c;
}

void kick() { rsv::watch_kick(); }

void update_line(uint32_t ea, bool (*fn)(uint8_t* line, uint32_t off, void* user), void* user) {
    uint32_t line = ea & ~127u;
    alignas(16) uint8_t old_data[128], new_data[128];
    for (;;) {
        uint64_t rtime = rsv::snapshot(line, old_data);
        std::memcpy(new_data, old_data, 128);
        if (!fn(new_data, ea & 127u, user)) return;
        if (rsv::commit_if(line, rtime, old_data, new_data, 0, 128)) return;
        _mm_pause();
    }
}

void dep_decrement(uint32_t ea) {
    struct K {
        bool fire = false;
        uint32_t wid = 0;
        uint8_t value = 0;
    } k;
    update_line(ea, [](uint8_t* l, uint32_t off, void* u) {
        auto* k = static_cast<K*>(u);
        uint16_t cnt = be16(l + off);
        wbe16(l + off, static_cast<uint16_t>(cnt - 1));
        k->fire = cnt == 1;
        k->wid = l[off + 2];
        k->value = l[off + 3];
        return true;
    }, &k);
    if (k.fire && k.value && k.wid < 32) {
        struct V {
            uint8_t v;
        } v{k.value};
        update_line(kSpurs + (k.wid < 16 ? 0 : 0x10) + (k.wid & 15), [](uint8_t* l, uint32_t off, void* u) {
            l[off] = static_cast<V*>(u)->v;
            return true;
        }, &v);
    }
}

uint32_t rc_off(uint32_t wid) { return (wid < 16 ? 0 : 0x10) + (wid & 15); }

enum Claim { C_NONE, C_BLOCKED, C_JOB, C_UNSUPPORTED };

struct Kind {
    enum K { EDGE, AB, CR } k = EDGE;
    Cmd cmd;
    uint32_t ab_counter = 0;
};

bool classify(uint32_t cmd_ea, uint32_t cmd_size, Kind& out) {
    if (!parse(cmd_ea, cmd_size, out.cmd)) return false;
    const Cmd& c = out.cmd;
    if (c.module == kEdgeEa) {
        out.k = Kind::EDGE;
        return edge_shape(c);
    }
    if (c.module == kAbEa) {
        out.k = Kind::AB;
        if (c.param_off + 16 > c.size) return false;
        const uint8_t* p = c.raw + c.param_off;
        if (be32(p + 4)) return false;
        out.ab_counter = be32(p + 12);
        return true;
    }
    if (c.module == kCrEa) {
        out.k = Kind::CR;
        return true;
    }
    return false;
}

struct Claimed {
    JobRef ref;
    Kind kind;
};

Claim try_claim(List& L, uint32_t v, Claimed& out) {
    alignas(16) uint8_t old_data[128], nd[128];
    for (int guard = 0; guard < 100000; guard++) {
        uint64_t rt = rsv::snapshot(L.hdr, old_data);
        uint32_t blk = be32(old_data + 8);
        if (blk > 3) return C_UNSUPPORTED;
        uint8_t* q = old_data + 0x40 + 16 * blk;
        uint16_t a = be16(q), b = be16(q + 2);
        uint32_t arr = be32(old_data + 0x30 + 4 * blk);
        if (!arr || !vm::range_mapped(arr + 8u * b, 8)) return C_UNSUPPORTED;
        uint64_t e = vm::read64(arr + 8u * b);
        uint32_t type = static_cast<uint32_t>(e >> 32) & 0xffff;
        if (type == 0) return C_NONE;
        std::memcpy(nd, old_data, 128);
        uint8_t* nq = nd + 0x40 + 16 * blk;
        uint16_t nb = static_cast<uint16_t>(b + 1);
        wbe16(nq, a > nb ? a : nb);
        wbe16(nq + 2, nb);
        if (type == 1) {
            uint32_t size = static_cast<uint32_t>(e >> 48);
            if (!classify(static_cast<uint32_t>(e), size, out.kind)) return C_UNSUPPORTED;
            uint16_t slot = be16(q + 4 + 2 * v);
            if (slot > b) wbe16(nq + 4 + 2 * v, b);
        } else if (type == 2) {
            uint32_t ea = static_cast<uint32_t>(e);
            if (!vm::range_mapped(ea & ~1u, 2)) return C_UNSUPPORTED;
            if (vm::read16(ea)) return C_BLOCKED;
        } else if (type != 3) {
            return C_UNSUPPORTED;
        }
        if (!rsv::commit_if(L.hdr, rt, old_data, nd, 0, 128)) {
            _mm_pause();
            continue;
        }
        if (type == 1) {
            out.ref.hdr = L.hdr;
            out.ref.block = blk;
            out.ref.wid = L.wid;
            out.ref.index = b;
            out.ref.spurs = kSpurs;
            out.ref.arr = arr;
            return C_JOB;
        }
        (type == 2 ? g_st.barriers : g_st.nops).fetch_add(1, std::memory_order_relaxed);
    }
    return C_BLOCKED;
}

void release_slot(List& L, uint32_t v) {
    alignas(16) uint8_t old_data[128], nd[128];
    for (;;) {
        uint64_t rt = rsv::snapshot(L.hdr, old_data);
        uint32_t blk = be32(old_data + 8) & 3;
        std::memcpy(nd, old_data, 128);
        wbe16(nd + 0x40 + 16 * blk + 4 + 2 * v, 0xffff);
        if (rsv::commit_if(L.hdr, rt, old_data, nd, 0, 128)) return;
        _mm_pause();
    }
}

uint16_t list_min(const uint8_t* line) {
    uint32_t blk = be32(line + 8) & 3;
    const uint8_t* q = line + 0x40 + 16 * blk;
    uint16_t m = be16(q + 2);
    for (int k = 0; k < 6; k++) m = std::min<uint16_t>(m, be16(q + 4 + 2 * k));
    return m;
}

void run_ab(List& L, const Claimed& c) {
    uint64_t t0 = __rdtsc();
    alignas(16) uint8_t line[128];
    for (uint32_t i = 0;; i++) {
        rsv::snapshot(L.hdr, line);
        if (list_min(line) >= c.ref.index) break;
        if (i < 64) _mm_pause();
        else if (i < 4096) SwitchToThread();
        else Sleep(0);
    }
    g_st.ab_wait.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
    if (c.kind.ab_counter) dep_decrement(c.kind.ab_counter);
}

void emit_job(uint64_t t0, uint32_t v, uint32_t wid, uint32_t module) {
    if (!critpath::g_on) return;
    critpath::Ev e{};
    e.type = critpath::EV_JOB;
    e.t0 = t0;
    e.t1 = __rdtsc();
    e.a = module;
    e.b = wid & 0xff;
    e.c = (0x40 + v) | (1u << 8);
    critpath::push(e);
}

void store_event(uint32_t ea, uint32_t v, uint32_t wid, uint32_t module) {
    if (!critpath::g_on) return;
    critpath::SpuState s;
    s.open = true;
    s.src = module;
    s.wid = wid;
    critpath::spu_stored(s, 0x40 + v, ea, 2);
}

bool run_claimed(List& L, uint32_t v, Claimed& c) {
    uint64_t t0 = __rdtsc(), c0 = thread_cycles();
    bool ok = true;
    switch (c.kind.k) {
    case Kind::EDGE: {
        Executor& x = *g_vspu[v].x;
        HostCounters before = x.hc;
        ok = x.run_edge(c.ref, c.kind.cmd, cfg().reference);
        g_st.edge.fetch_add(1, std::memory_order_relaxed);
        g_st.gets.fetch_add(x.hc.gets - before.gets, std::memory_order_relaxed);
        g_st.get_bytes.fetch_add(x.hc.get_bytes - before.get_bytes, std::memory_order_relaxed);
        g_st.puts.fetch_add(x.hc.puts - before.puts, std::memory_order_relaxed);
        g_st.put_bytes.fetch_add(x.hc.put_bytes - before.put_bytes, std::memory_order_relaxed);
        g_st.slow.fetch_add(x.hc.slow - before.slow, std::memory_order_relaxed);
        if (!ok) {
            g_st.failed.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 20)
                LOG_ERROR("jsedge", "edge job wid %u idx %u cmd 0x%08x on vspu %u ended at pc 0x%05x (api bad pc 0x%05x)", c.ref.wid, c.ref.index, c.kind.cmd.ea, v, x.fail_pc, x.api.bad_pc);
        }
        break;
    }
    case Kind::AB:
        run_ab(L, c);
        g_st.ab.fetch_add(1, std::memory_order_relaxed);
        break;
    case Kind::CR:
        g_st.cr.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    release_slot(L, v);
    for (uint32_t i = 0; i < c.kind.cmd.ndeps; i++) {
        dep_decrement(c.kind.cmd.deps[i]);
        store_event(c.kind.cmd.deps[i], v, L.wid, c.kind.cmd.module);
    }
    uint64_t t1 = __rdtsc();
    if (c.kind.k == Kind::EDGE) {
        g_st.tsc.fetch_add(t1 - t0, std::memory_order_relaxed);
        g_st.cpu.fetch_add(thread_cycles() - c0, std::memory_order_relaxed);
    }
    emit_job(t0, v, L.wid, c.kind.cmd.module);
    kick();
    return ok;
}

void handback(List& L, const char* why, uint32_t idx) {
    int expected = S_OWNED;
    if (!L.state.compare_exchange_strong(expected, S_HANDBACK)) return;
    g_st.unsupported.fetch_add(1, std::memory_order_relaxed);
    LOG_WARN("jsedge", "wid %u entry %u: %s, handing the list back to the SPUs", L.wid, idx, why);
}

bool scan(List& L, uint32_t v) {
    if (L.state.load(std::memory_order_acquire) != S_OWNED) return false;
    alignas(16) uint8_t sp[128], nsp[128];
    uint64_t rt = rsv::snapshot(kSpurs, sp);
    uint32_t off = rc_off(L.wid);
    if (!sp[off]) return false;
    L.held.fetch_add(1, std::memory_order_acq_rel);
    if (L.state.load(std::memory_order_acquire) != S_OWNED) {
        L.held.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }
    Claimed c;
    Claim r = try_claim(L, v, c);
    if (r == C_JOB) {
        run_claimed(L, v, c);
        L.held.fetch_sub(1, std::memory_order_acq_rel);
        return true;
    }
    if (r == C_UNSUPPORTED) {
        alignas(16) uint8_t line[128];
        rsv::snapshot(L.hdr, line);
        handback(L, "unsupported entry", be16(line + 0x40 + 16 * (be32(line + 8) & 3) + 2));
        L.held.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }
    if (r == C_BLOCKED) g_st.blocked.fetch_add(1, std::memory_order_relaxed);
    std::memcpy(nsp, sp, 128);
    nsp[off] = 0;
    bool zeroed = rsv::commit_if(kSpurs, rt, sp, nsp, 0, 128);
    if (zeroed) g_st.rc_zero.fetch_add(1, std::memory_order_relaxed);
    r = try_claim(L, v, c);
    if (r == C_JOB) {
        run_claimed(L, v, c);
        L.held.fetch_sub(1, std::memory_order_acq_rel);
        return true;
    }
    L.held.fetch_sub(1, std::memory_order_acq_rel);
    return !zeroed;
}

int grab_vspu() {
    for (uint32_t i = 0; i < cfg().vspus; i++) {
        bool f = false;
        if (!g_vspu[i].busy.load(std::memory_order_relaxed) && g_vspu[i].busy.compare_exchange_strong(f, true, std::memory_order_acquire)) return static_cast<int>(i);
    }
    return -1;
}

bool work_once() {
    int v = grab_vspu();
    if (v < 0) return false;
    bool did = false;
    for (uint32_t k = 0; k < kLists; k++) {
        if (scan(g_lists[k], static_cast<uint32_t>(v))) {
            did = true;
            break;
        }
    }
    g_vspu[v].busy.store(false, std::memory_order_release);
    return did;
}

bool any_ready() {
    for (List& L : g_lists) {
        if (L.state.load(std::memory_order_relaxed) != S_OWNED) continue;
        if (vm::ptr(kSpurs)[rc_off(L.wid)]) return true;
    }
    return false;
}

void park(uint32_t ms) {
    uint32_t w0 = rsv::g_watch_word.load(std::memory_order_seq_cst);
    rsv::g_watch_sleepers.fetch_add(1, std::memory_order_seq_cst);
    if (!any_ready()) WaitOnAddress(&rsv::g_watch_word, &w0, sizeof w0, ms);
    rsv::g_watch_sleepers.fetch_sub(1, std::memory_order_seq_cst);
}

void worker_main(int k) {
    wchar_t w[64];
    swprintf(w, 64, L"SPU edge worker %d", k);
    SetThreadDescription(GetCurrentThread(), w);
    if (k < static_cast<int>(cfg().cpus.size()) && cfg().cpus[k] >= 0 && cfg().cpus[k] < 64) SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << cfg().cpus[k]);
    if (cfg().prio_set) SetThreadPriority(GetCurrentThread(), cfg().prio);
    _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x6000u | 0x8040u);
    spu::hle::install_builtin();
    for (uint32_t idle = 0;;) {
        if (work_once()) {
            idle = 0;
            continue;
        }
        if (++idle < 32) {
            for (int i = 0; i < 64; i++) _mm_pause();
            continue;
        }
        park(1);
    }
}

void set_prio(List& L, const uint8_t* p) {
    rsv::store_locked(L.info + 0x18, p, 8);
    uint8_t ff = 0xFF;
    rsv::store_locked(kSpurs + 0xBD, &ff, 1);
    rsv::store_locked(kSpurs + 0x72, &ff, 1);
}

bool spus_out(const List& L) {
    uint8_t cont = vm::ptr(kSpurs)[0x20 + (L.wid & 15)];
    if ((L.wid < 16 ? cont & 15 : cont >> 4) != 0) return false;
    alignas(16) uint8_t line[128];
    rsv::snapshot(L.hdr, line);
    uint32_t blk = be32(line + 8) & 3;
    for (int k = 0; k < 6; k++)
        if (be16(line + 0x40 + 16 * blk + 4 + 2 * k) != 0xffff) return false;
    return true;
}

void manage() {
    for (uint32_t k = 0; k < kLists; k++) {
        List& L = g_lists[k];
        int s = L.state.load(std::memory_order_acquire);
        if (s == S_SPU) {
            L.wid = kFirstWid + k;
            if (!vm::range_mapped(kSpurs, 0x1100)) continue;
            if ((vm::read32(kSpurs + 0xB0) & (0x80000000u >> L.wid)) == 0) continue;
            L.info = kSpurs + 0x1000 + (L.wid & 15) * 0x20;
            if (vm::read32(L.info + 4) != kPmEa) continue;
            L.hdr = vm::read32(L.info + 0xc);
            if (!L.hdr || (L.hdr & 127) || !vm::range_mapped(L.hdr, 128)) continue;
            uint64_t p = vm::read64(L.info + 0x18);
            if (!p) continue;
            for (int i = 0; i < 8; i++) L.prio_saved[i] = static_cast<uint8_t>(p >> (56 - 8 * i));
            uint8_t zero[8] = {};
            set_prio(L, zero);
            L.stable = 0;
            L.state.store(S_PENDING, std::memory_order_release);
            LOG_INFO("jsedge", "wid %u header 0x%08x: SPU priority %u,%u,%u,%u,%u,%u -> 0, waiting for the SPUs to leave", L.wid, L.hdr, L.prio_saved[0], L.prio_saved[1], L.prio_saved[2], L.prio_saved[3], L.prio_saved[4], L.prio_saved[5]);
        } else if (s == S_PENDING) {
            if (spus_out(L)) {
                if (++L.stable >= 3) {
                    L.state.store(S_OWNED, std::memory_order_release);
                    LOG_INFO("jsedge", "wid %u owned by the host consumer", L.wid);
                    kick();
                }
            } else {
                L.stable = 0;
            }
        } else if (s == S_HANDBACK) {
            if (L.held.load(std::memory_order_acquire) == 0) {
                set_prio(L, L.prio_saved);
                L.state.store(S_DISABLED, std::memory_order_release);
                LOG_WARN("jsedge", "wid %u handed back to the SPUs", L.wid);
            }
        }
    }
}

void stats_line() {
    static uint64_t last[16] = {};
    static double hz = [] {
        auto c0 = std::chrono::steady_clock::now();
        uint64_t t0 = __rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return double(__rdtsc() - t0) / std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
    }();
    uint64_t v[16] = {g_st.edge.load(), g_st.ab.load(), g_st.cr.load(), g_st.tsc.load(), g_st.cpu.load(), g_st.helped.load(), g_st.blocked.load(), g_st.rc_zero.load(),
                      g_st.gets.load(), g_st.get_bytes.load(), g_st.puts.load(), g_st.put_bytes.load(), g_st.slow.load(), g_st.failed.load(), g_st.ab_wait.load(), g_st.nops.load()};
    uint64_t d[16];
    for (int i = 0; i < 16; i++) d[i] = v[i] - last[i];
    std::memcpy(last, v, sizeof v);
    double jn = d[0] ? double(d[0]) : 1.0;
    LOG_INFO("jsedge", "5s: edge %llu ab %llu cr %llu (helped %llu), %.1f us/job wall %.1f us/job cpu, blocked %llu rc-zero %llu, ab wait %.2f ms, failed %llu total, slow dma %llu",
             static_cast<unsigned long long>(d[0]), static_cast<unsigned long long>(d[1]), static_cast<unsigned long long>(d[2]), static_cast<unsigned long long>(d[5]), double(d[3]) / hz * 1e6 / jn,
             double(d[4]) / hz * 1e6 / jn, static_cast<unsigned long long>(d[6]), static_cast<unsigned long long>(d[7]), double(d[14]) / hz * 1e3, static_cast<unsigned long long>(v[13]),
             static_cast<unsigned long long>(d[12]));
    LOG_INFO("jsedge", "5s dma per edge job: %.1f GETs %.0f bytes, %.1f PUTs %.0f bytes; nops %llu", double(d[8]) / jn, double(d[9]) / jn, double(d[10]) / jn, double(d[11]) / jn, static_cast<unsigned long long>(d[15]));
    if (cfg().verify)
        LOG_INFO("jsedge", "verify: loader %llu/%llu bad, exec %llu/%llu bad (%llu skipped, %llu bytes compared)", static_cast<unsigned long long>(g_st.lv_bad.load()), static_cast<unsigned long long>(g_st.lv_runs.load()),
                 static_cast<unsigned long long>(g_st.xv_bad.load()), static_cast<unsigned long long>(g_st.xv_runs.load()), static_cast<unsigned long long>(g_st.xv_skip.load()),
                 static_cast<unsigned long long>(g_st.xv_bytes.load()));
}

void manager_main() {
    SetThreadDescription(GetCurrentThread(), L"edge consumer manager");
    if (rsv::g_watch_line.load() == 0xFFFFFFFFu) rsv::g_watch_line.store(kSpurs);
    auto next = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        if (cfg().on) manage();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (std::chrono::steady_clock::now() >= next) {
            next += std::chrono::seconds(5);
            stats_line();
        }
    }
}

struct Probe {
    bool active = false;
    Sandbox sb;
    uint32_t cmd = 0;
};
thread_local Probe t_probe;
thread_local std::unique_ptr<spu::SpuThread> t_shadow;
std::atomic<uint64_t> g_probe_seq{0};

spu::SpuThread* owner(spu::Context& c) {
    return reinterpret_cast<spu::SpuThread*>(reinterpret_cast<char*>(&c) - offsetof(spu::SpuThread, ctx));
}

uint32_t compare_ranges(const uint8_t* a, const uint8_t* b, const std::vector<std::pair<uint32_t, uint32_t>>& r, uint32_t& first) {
    uint32_t bad = 0;
    for (auto& [lo, hi] : r) {
        for (uint32_t x = lo; x < hi && x < spu::LS_SIZE; x++) {
            if (a[x] != b[x]) {
                if (!bad) first = x;
                bad++;
            }
        }
    }
    return bad;
}

bool probe_entry(spu::Context& c) {
    if (!cfg().verify || (g_probe_seq.fetch_add(1, std::memory_order_relaxed) % cfg().verify)) return false;
    const uint8_t* ls = c.ls;
    if (std::memcmp(ls + 0x4000, vm::ptr(kEdgeEa), 0x30) != 0) return false;
    const uint8_t* api = ls + kApiLs;
    JobRef j;
    uint32_t w10 = be32(api + 0x10);
    j.wid = w10 >> 16;
    j.index = w10 & 0xffff;
    uint32_t hb = be32(api + 0x70);
    j.hdr = hb & ~127u;
    j.block = hb & 3;
    j.spurs = be32(api + 0x0c);
    if (!vm::range_mapped(j.hdr, 128)) return false;
    j.arr = vm::read32(j.hdr + 0x30 + 4 * j.block);
    if (!vm::range_mapped(j.arr + 8 * j.index, 8)) return false;
    uint64_t e = vm::read64(j.arr + 8 * j.index);
    Kind kd;
    if (!classify(static_cast<uint32_t>(e), static_cast<uint32_t>(e >> 48), kd) || kd.k != Kind::EDGE) {
        g_st.xv_skip.fetch_add(1);
        return false;
    }
    const Cmd& cmd = kd.cmd;
    const BufSet& s1 = cmd.sets[1];
    uint32_t buf = 2;
    for (uint32_t b = 0; b < 2; b++) {
        uint32_t base = set_base(cmd, 1, b);
        uint32_t list_ls = base + s1.pages * 0x400u - ((s1.len + 15) & ~15u);
        if (std::memcmp(ls + list_ls, vm::ptr(s1.ea), s1.len) == 0) buf = b;
    }
    if (buf > 1) {
        g_st.xv_skip.fetch_add(1);
        return false;
    }
    uint32_t spu_num = be32(ls + 0x1c8);
    std::vector<uint8_t> mine(ls, ls + spu::LS_SIZE);
    spu::Context mc = c;
    mc.ls = mine.data();
    prepare_ls(mine.data(), mc, j, cmd, spu_num, buf, true, false);
    std::vector<std::pair<uint32_t, uint32_t>> rg = {{0x4000, 0x4000 + 0xc430}, {kCmdLs, kCmdLs + cmd.size}, {kApiLs, kApiLs + 0x54}, {kApiLs + 0xb0, kApiLs + 0xb4}, {0x1c8, 0x1cc}};
    uint32_t base = set_base(cmd, 1, buf);
    uint32_t list_ls = base + s1.pages * 0x400u - ((s1.len + 15) & ~15u);
    rg.push_back({list_ls, list_ls + s1.len});
    uint32_t cur = base;
    for (uint32_t n = 0; n + 8 <= s1.len; n += 8) {
        const uint8_t* el = vm::ptr(s1.ea + n);
        uint32_t ts = ((uint32_t(el[2]) << 8) | el[3]) & 0x7FFF;
        uint32_t ea = be32(el + 4);
        uint32_t at = cur | (ea & 15);
        rg.push_back({at, at + ts});
        cur = (cur + ((ts + 15) & ~15u)) & 0x3FFF0;
    }
    uint32_t first = 0;
    uint32_t bad = compare_ranges(mine.data(), ls, rg, first);
    int rbad = 0;
    for (int r : {0, 3, 4, 5, 6}) {
        if (std::memcmp(&mc.gpr[r], &c.gpr[r], 16) != 0) rbad |= 1 << r;
    }
    if (c.gpr[1].u32[3] != mc.gpr[1].u32[3]) rbad |= 2;
    g_st.lv_runs.fetch_add(1);
    if (bad || rbad) {
        g_st.lv_bad.fetch_add(1);
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 20) LOG_ERROR("jsedge", "loader verify MISMATCH job wid %u idx %u cmd 0x%08x: %u LS bytes differ (first 0x%05x: mine %02x spu %02x), reg mask 0x%x", j.wid, j.index, cmd.ea, bad, first, mine[first], ls[first], rbad);
    }
    if (!t_shadow) t_shadow = std::make_unique<spu::SpuThread>();
    spu::SpuThread& s = *t_shadow;
    std::memcpy(s.ls, ls, spu::LS_SIZE);
    std::memcpy(s.ctx.gpr, c.gpr, sizeof c.gpr);
    s.ctx.pc = c.pc;
    s.ctx.interrupts_enabled = c.interrupts_enabled;
    s.tag_mask = s.tag_stat = s.atomic_stat = 0;
    s.tag_stat_count = s.atomic_stat_count = false;
    s.raddr = 0;
    ApiState st;
    for (int k = 0; k < 4; k++) st.mask[k] = 0xff;
    st.buf = buf;
    st.cmd = &cmd;
    t_probe = Probe{};
    bool ok = run_sandboxed(s, st, t_probe.sb);
    if (!ok) {
        g_st.xv_skip.fetch_add(1);
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 10) LOG_WARN("jsedge", "exec verify skipped: pc 0x%05x api bad 0x%05x abort ch 0x%x", s.ctx.pc, st.bad_pc, t_probe.sb.abort_ch);
        return false;
    }
    t_probe.active = true;
    t_probe.cmd = cmd.ea;
    return false;
}

bool probe_return(spu::Context&) {
    if (!t_probe.active) return false;
    t_probe.active = false;
    uint32_t bad = 0, first = 0;
    for (auto& [a, b] : t_probe.sb.writes) {
        if (vm::ptr(a)[0] != b) {
            if (!bad) first = a;
            bad++;
        }
    }
    g_st.xv_runs.fetch_add(1);
    g_st.xv_bytes.fetch_add(t_probe.sb.writes.size());
    static std::atomic<int> logged{0};
    if (bad) {
        g_st.xv_bad.fetch_add(1);
        if (logged.fetch_add(1) < 20) LOG_ERROR("jsedge", "exec verify MISMATCH cmd 0x%08x: %u of %zu bytes differ, first ea 0x%08x (mine %02x real %02x)", t_probe.cmd, bad, t_probe.sb.writes.size(), first, t_probe.sb.writes[first], vm::ptr(first)[0]);
    } else if (logged.load() < 10) {
        logged.fetch_add(1);
        LOG_INFO("jsedge", "exec verify ok cmd 0x%08x: %zu bytes, %u atomics", t_probe.cmd, t_probe.sb.writes.size(), t_probe.sb.atomics);
    }
    t_probe.sb.writes.clear();
    return false;
}

const uint32_t kEntrySig[16] = {0x24ffc0d0, 0x24ff80d1, 0x24ff40d2, 0x24ff00fe, 0x24004080, 0x24fe8081, 0x1ce80081, 0x1c000251,
                                0x01a00452, 0x238ba883, 0x40800005, 0x238ba985, 0x4236e802, 0x330000fe, 0x081f817e, 0x42659003};
const uint32_t kRetSig[8] = {0x40804000, 0x30826402, 0x40800103, 0x3082ce04, 0x21a00b00, 0x30873c00, 0x34000102, 0x21a00b83};

}

bool enabled() { return cfg().on || cfg().verify; }

void init() {
    if (!enabled()) return;
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;
    if (cfg().verify) {
        spu::hle::install_builtin();
        spu::hle::register_function({0x3368, 8, kRetSig, probe_return, "EdgeConsumer.verify_ret"});
        spu::hle::register_function({kEntryLs, 16, kEntrySig, probe_entry, "EdgeConsumer.verify_entry"});
    }
    if (!cfg().on) {
        std::thread(manager_main).detach();
        return;
    }
    for (uint32_t i = 0; i < kVspus; i++) g_vspu[i].x = std::make_unique<Executor>(i);
    LOG_INFO("jsedge", "Edge list consumer on: %d workers, %u virtual SPUs, help %d, reference %d, verify %u", cfg().threads, cfg().vspus, cfg().help ? 1 : 0, cfg().reference ? 1 : 0, cfg().verify);
    std::thread(manager_main).detach();
    for (int k = 0; k < cfg().threads; k++) std::thread(worker_main, k).detach();
}

bool on_wait_previous(ppu::Context&) {
    if (!cfg().on || !cfg().help) return false;
    uint32_t cur = vm::read32(0xa0c13c);
    if (cur > 2) return false;
    uint32_t prev = (cur + 2) % 3;
    if (!*vm::ptr(0xa0c12b + prev)) return false;
    uint32_t counter = 0xa0b740 + 4 * prev;
    if (!vm::read16(counter)) return false;
    static thread_local bool csr = false;
    unsigned saved = _mm_getcsr();
    if (!csr) spu::hle::install_builtin();
    csr = true;
    _mm_setcsr((saved & ~0x6000u) | 0x6000u | 0x8040u);
    for (uint32_t i = 0; vm::read16(counter); i++) {
        uint64_t before = g_st.edge.load(std::memory_order_relaxed);
        if (work_once()) {
            if (g_st.edge.load(std::memory_order_relaxed) != before) g_st.helped.fetch_add(1, std::memory_order_relaxed);
            i = 0;
            continue;
        }
        if (i < 64) _mm_pause();
        else if (i < 2048) SwitchToThread();
        else break;
    }
    _mm_setcsr(saved);
    return false;
}

}
