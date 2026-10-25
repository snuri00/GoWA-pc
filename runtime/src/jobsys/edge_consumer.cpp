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

#if __has_include("runtime/jobsys.h")
#include "runtime/jobsys.h"
#define JSEDGE_CORE 1
#endif

namespace crash {
void profile_thread(void* native, const char* name);
}

namespace jsedge {

namespace {

constexpr uint32_t kSpurs = 0x3001ad00;
constexpr uint32_t kVspus = 4;
constexpr uint32_t kFirstWid = 19;
constexpr uint32_t kLists = 3;

struct Config {
    bool on = false;
    int threads = 0;
    std::vector<int> cpus;
    int prio = 0;
    bool prio_set = false;
    bool help = true;
    uint32_t verify = 0;
    bool reference = false;
    bool donate = true;
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
        if (const char* d = std::getenv("GOWA_JOBSYS_EDGE_DONATE")) c.donate = d[0] != '0';
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
    std::atomic<uint64_t> cool_until{0};
    std::unique_ptr<Executor> x;
};

uint64_t g_cool_cycles = 0;

VSpu g_vspu[kVspus];

struct Stats {
    std::atomic<uint64_t> edge{0}, ab{0}, cr{0}, nops{0}, barriers{0}, blocked{0}, rc_zero{0}, helped{0}, failed{0}, unsupported{0};
    std::atomic<uint64_t> tsc{0}, cpu{0}, cpu_n{0}, ab_wait{0}, prio_fix{0}, prio_hooked{0}, donated{0}, idle_calls{0}, cooled{0};
    std::atomic<uint64_t> gets{0}, get_bytes{0}, puts{0}, put_bytes{0}, slow{0}, insns{0}, disp{0}, fb{0}, decr{0}, labg{0}, othr{0};
    std::atomic<uint64_t> lv_runs{0}, lv_bad{0}, xv_runs{0}, xv_bad{0}, xv_skip{0}, xv_bytes{0}, xv_ringb{0}, sc_runs{0}, sc_bad{0}, sc_skip{0}, sc_bytes{0}, sc_ringb{0};
};
Stats g_st;

struct FreshStat {
    std::atomic<uint64_t> runs{0}, differ{0}, skipped{0};
};
FreshStat g_fresh[256];
std::atomic<uint64_t> g_spu_edge[64];
std::atomic<uint64_t> g_spu_water[64];


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

void self_check(Sandbox& sb, const Claimed& c) {
    if (sb.abort) {
        g_st.sc_skip.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint32_t rings = be32(c.kind.cmd.raw + c.kind.cmd.param_off) + 0x80;
    uint32_t bad = 0, first = 0, ringb = 0;
    for (auto& [a, b] : sb.writes) {
        if (vm::ptr(a)[0] == b) continue;
        if (a - rings < 0x200 && (a & 0x7f) - 0x18 < 0x18) {
            ringb++;
            continue;
        }
        if (!bad) first = a;
        bad++;
    }
    g_st.sc_runs.fetch_add(1, std::memory_order_relaxed);
    g_st.sc_bytes.fetch_add(sb.writes.size(), std::memory_order_relaxed);
    if (ringb) g_st.sc_ringb.fetch_add(1, std::memory_order_relaxed);
    if (bad) {
        g_st.sc_bad.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 20) LOG_ERROR("jsedge", "self-check MISMATCH cmd 0x%08x: %u of %zu bytes differ, first ea 0x%08x (sandbox %02x live %02x)", c.kind.cmd.ea, bad, sb.writes.size(), first, sb.writes[first], vm::ptr(first)[0]);
    }
}

bool run_claimed(List& L, uint32_t v, Claimed& c) {
    static thread_local uint32_t sample = 0;
    bool timed = (sample++ & 15) == 0;
    uint64_t t0 = __rdtsc(), c0 = timed ? thread_cycles() : 0;
    bool ok = true;
    switch (c.kind.k) {
    case Kind::EDGE: {
        Executor& x = *g_vspu[v].x;
        HostCounters before = x.hc;
        uint64_t n0 = x.thread().exec_insns, d0 = x.dispatches, f0 = x.fallbacks;
        static std::atomic<uint64_t> seq{0};
        bool check = cfg().verify && cfg().on && seq.fetch_add(1, std::memory_order_relaxed) % cfg().verify == 0;
        Sandbox sb;
        ok = x.run_edge(c.ref, c.kind.cmd, cfg().reference, check ? &sb : nullptr);
        if (check) self_check(sb, c);
        if (g_cool_cycles && be32(x.thread().ls + kApiLs + 0xa0) == 0) {
            g_vspu[v].cool_until.store(__rdtsc() + g_cool_cycles, std::memory_order_relaxed);
            g_st.cooled.fetch_add(1, std::memory_order_relaxed);
        }
        g_st.insns.fetch_add(x.thread().exec_insns - n0, std::memory_order_relaxed);
        g_st.disp.fetch_add(x.dispatches - d0, std::memory_order_relaxed);
        g_st.fb.fetch_add(x.fallbacks - f0, std::memory_order_relaxed);
        g_st.edge.fetch_add(1, std::memory_order_relaxed);
        g_st.gets.fetch_add(x.hc.gets - before.gets, std::memory_order_relaxed);
        g_st.get_bytes.fetch_add(x.hc.get_bytes - before.get_bytes, std::memory_order_relaxed);
        g_st.puts.fetch_add(x.hc.puts - before.puts, std::memory_order_relaxed);
        g_st.put_bytes.fetch_add(x.hc.put_bytes - before.put_bytes, std::memory_order_relaxed);
        g_st.slow.fetch_add(x.hc.slow - before.slow, std::memory_order_relaxed);
        g_st.decr.fetch_add(x.hc.dec_reads - before.dec_reads, std::memory_order_relaxed);
        g_st.labg.fetch_add(x.hc.label_gets - before.label_gets, std::memory_order_relaxed);
        g_st.othr.fetch_add(x.hc.other_reads - before.other_reads, std::memory_order_relaxed);
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
        if (timed) {
            g_st.cpu.fetch_add(thread_cycles() - c0, std::memory_order_relaxed);
            g_st.cpu_n.fetch_add(1, std::memory_order_relaxed);
        }
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

std::atomic<uint32_t> g_rr{0};

bool try_take(uint32_t i, uint64_t now) {
    if (g_vspu[i].busy.load(std::memory_order_relaxed) || g_vspu[i].cool_until.load(std::memory_order_relaxed) > now) return false;
    bool f = false;
    return g_vspu[i].busy.compare_exchange_strong(f, true, std::memory_order_acquire);
}

int grab_vspu(int pref = -1) {
    uint32_t n = cfg().vspus;
    uint64_t now = __rdtsc();
    if (pref >= 0 && static_cast<uint32_t>(pref) < n && try_take(static_cast<uint32_t>(pref), now)) return pref;
    uint32_t start = g_rr.fetch_add(1, std::memory_order_relaxed);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t i = (start + k) % n;
        if (try_take(i, now)) return static_cast<int>(i);
    }
    return -1;
}

bool work_once(int pref = -1) {
    int v = grab_vspu(pref);
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
    if (k == 0 && std::getenv("GOWA_JOBSYS_EDGE_PROFILE")) {
        HANDLE h = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
        crash::profile_thread(h, "edge worker 0");
    }
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
        } else if (s == S_OWNED) {
            if (vm::read64(L.info + 0x18)) {
                uint64_t p = vm::read64(L.info + 0x18);
                for (int i = 0; i < 8; i++) L.prio_saved[i] = static_cast<uint8_t>(p >> (56 - 8 * i));
                uint8_t zero[8] = {};
                set_prio(L, zero);
                L.stable = 0;
                L.state.store(S_PENDING, std::memory_order_release);
                g_st.prio_fix.fetch_add(1, std::memory_order_relaxed);
                static std::atomic<int> logged{0};
                if (logged.fetch_add(1) < 8) LOG_WARN("jsedge", "wid %u priority was set to %u,%u,%u,%u behind the consumer, zeroed again, waiting for the SPUs to leave", L.wid, L.prio_saved[0], L.prio_saved[1], L.prio_saved[2], L.prio_saved[3]);
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
    static uint64_t last_cn = 0;
    uint64_t cn = g_st.cpu_n.load();
    double cpu_jn = cn - last_cn ? double(cn - last_cn) : 1.0;
    last_cn = cn;
    LOG_INFO("jsedge", "5s: edge %llu ab %llu cr %llu (helped %llu), %.1f us/job wall %.1f us/job cpu, blocked %llu rc-zero %llu, ab wait %.2f ms, failed %llu total, slow dma %llu",
             static_cast<unsigned long long>(d[0]), static_cast<unsigned long long>(d[1]), static_cast<unsigned long long>(d[2]), static_cast<unsigned long long>(d[5]), double(d[3]) / hz * 1e6 / jn,
             double(d[4]) / hz * 1e6 / cpu_jn, static_cast<unsigned long long>(d[6]), static_cast<unsigned long long>(d[7]), double(d[14]) / hz * 1e3, static_cast<unsigned long long>(v[13]),
             static_cast<unsigned long long>(d[12]));
    static uint64_t last_insns = 0;
    uint64_t ins = g_st.insns.load();
    static uint64_t last_disp = 0, last_fb = 0;
    uint64_t dsp = g_st.disp.load(), fbk = g_st.fb.load();
    LOG_INFO("jsedge", "5s dma per edge job: %.1f GETs %.0f bytes, %.1f PUTs %.0f bytes; nops %llu; exec %.1f kinsn/job, %.1f dispatches/job, %llu fallbacks", double(d[8]) / jn, double(d[9]) / jn, double(d[10]) / jn,
             double(d[11]) / jn, static_cast<unsigned long long>(d[15]), double(ins - last_insns) / 1e3 / jn, double(dsp - last_disp) / jn, static_cast<unsigned long long>(fbk - last_fb));
    last_insns = ins;
    last_disp = dsp;
    last_fb = fbk;
    static uint64_t l_dec = 0, l_lab = 0, l_oth = 0;
    uint64_t vd = g_st.decr.load(), vl = g_st.labg.load(), vo = g_st.othr.load();
    LOG_INFO("jsedge", "5s per edge job: %.1f decrementer reads, %.2f label GETs, %.1f other channel reads", double(vd - l_dec) / jn, double(vl - l_lab) / jn, double(vo - l_oth) / jn);
    l_dec = vd;
    l_lab = vl;
    l_oth = vo;
    {
        std::string a;
        for (int w = 0; w < 64; w++) {
            uint64_t e = g_spu_edge[w].exchange(0), x = g_spu_water[w].exchange(0);
            if (!e && !x) continue;
            char b[64];
            std::snprintf(b, sizeof b, " wid%d edge %llu water %llu", w, static_cast<unsigned long long>(e), static_cast<unsigned long long>(x));
            a += b;
        }
        if (!a.empty()) LOG_INFO("jsedge", "5s SPU-side entries:%s", a.c_str());
        static uint64_t l_don = 0;
        uint64_t dn = g_st.donated.load();
        static uint64_t l_idle = 0;
        uint64_t ic = g_st.idle_calls.load();
        static uint64_t l_cool = 0;
        uint64_t cl = g_st.cooled.load();
        LOG_INFO("jsedge", "priority calls intercepted %llu, re-zeroed %llu; SPU idle calls %llu, donated jobs %llu, ring-short cool-downs %llu", static_cast<unsigned long long>(g_st.prio_hooked.load()),
                 static_cast<unsigned long long>(g_st.prio_fix.load()), static_cast<unsigned long long>(ic - l_idle), static_cast<unsigned long long>(dn - l_don), static_cast<unsigned long long>(cl - l_cool));
        l_cool = cl;
        l_idle = ic;
        l_don = dn;
    }
    for (int m = 0; m < 256; m++)
        if (g_fresh[m].runs.load() || g_fresh[m].skipped.load())
            LOG_INFO("jsedge", "fresh mask 0x%x: %llu runs, %llu differ, %llu skipped", m, static_cast<unsigned long long>(g_fresh[m].runs.load()), static_cast<unsigned long long>(g_fresh[m].differ.load()),
                     static_cast<unsigned long long>(g_fresh[m].skipped.load()));
    if (cfg().verify && cfg().on)
        LOG_INFO("jsedge", "self-check: %llu/%llu bad (%llu skipped, %llu bytes, %llu ring-B pointer races)", static_cast<unsigned long long>(g_st.sc_bad.load()), static_cast<unsigned long long>(g_st.sc_runs.load()),
                 static_cast<unsigned long long>(g_st.sc_skip.load()), static_cast<unsigned long long>(g_st.sc_bytes.load()), static_cast<unsigned long long>(g_st.sc_ringb.load()));
    if (cfg().verify)
        LOG_INFO("jsedge", "verify: loader %llu/%llu bad, exec %llu/%llu bad (%llu skipped, %llu bytes compared, %llu ring-B pointer races)", static_cast<unsigned long long>(g_st.lv_bad.load()),
                 static_cast<unsigned long long>(g_st.lv_runs.load()), static_cast<unsigned long long>(g_st.xv_bad.load()), static_cast<unsigned long long>(g_st.xv_runs.load()),
                 static_cast<unsigned long long>(g_st.xv_skip.load()), static_cast<unsigned long long>(g_st.xv_bytes.load()), static_cast<unsigned long long>(g_st.xv_ringb.load()));
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
    uint32_t rings = 0;
};
thread_local Probe t_probe;
thread_local std::unique_ptr<spu::SpuThread> t_shadow;
std::atomic<uint64_t> g_probe_seq{0};


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

void fresh_check(spu::Context& c, const JobRef& j, const Cmd& cmd, uint32_t spu_num, uint32_t buf, uint32_t mask) {
    static thread_local std::unique_ptr<spu::SpuThread> f;
    if (!f) f = std::make_unique<spu::SpuThread>();
    spu::SpuThread& s = *f;
    const uint8_t* ls = c.ls;
    std::memset(s.ls, 0, spu::LS_SIZE);
    auto take = [&](uint32_t lo, uint32_t hi) { std::memcpy(s.ls + lo, ls + lo, hi - lo); };
    if (mask & 1) take(0, 0xa00);
    if (mask & 2) take(0xa00, 0x4000);
    if (mask & 4) take(0x10430, 0x11000);
    uint32_t cur = set_base(cmd, 1, buf), other = set_base(cmd, 1, buf ^ 1), len = cmd.sets[1].pages * 0x400u;
    if (mask & 8) take(other, other + len);
    if (mask & 0x10) take(cur, cur + len);
    if (mask & 0x20) take(0x29000, 0x3b000);
    if (mask & 0x40) take(0x3b000, 0x40000);
    if (mask & 0x80) std::memcpy(s.ctx.gpr, c.gpr, sizeof c.gpr);
    else std::memset(s.ctx.gpr, 0, sizeof s.ctx.gpr);
    prepare_ls(s.ls, s.ctx, j, cmd, spu_num, buf, true, !(mask & 2));
    s.tag_mask = s.tag_stat = s.atomic_stat = 0;
    s.tag_stat_count = s.atomic_stat_count = false;
    s.raddr = 0;
    ApiState st;
    for (int k = 0; k < 4; k++) st.mask[k] = 0xff;
    st.buf = buf;
    st.cmd = &cmd;
    Sandbox sb;
    FreshStat& fs = g_fresh[mask & 255];
    if (!run_sandboxed(s, st, sb)) {
        fs.skipped.fetch_add(1);
        return;
    }
    fs.runs.fetch_add(1);
    uint32_t bad = 0, first = 0;
    for (auto& [a, b] : t_probe.sb.writes) {
        auto it = sb.writes.find(a);
        if (it == sb.writes.end() || it->second != b) {
            if (!bad) first = a;
            bad++;
        }
    }
    for (auto& [a, b] : sb.writes)
        if (!t_probe.sb.writes.count(a)) bad++;
    if (bad) {
        fs.differ.fetch_add(1);
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 40) {
            uint8_t mine = sb.writes.count(first) ? sb.writes[first] : 0;
            LOG_WARN("jsedge", "fresh mask 0x%x cmd 0x%08x: %u bytes differ from the SPU-LS run, first ea 0x%08x (fresh %02x spu %02x), sizes %zu/%zu", mask, cmd.ea, bad, first, mine, t_probe.sb.writes[first], sb.writes.size(), t_probe.sb.writes.size());
        }
    }
}

spu::SpuThread* owner(spu::Context& c) {
    return reinterpret_cast<spu::SpuThread*>(reinterpret_cast<char*>(&c) - offsetof(spu::SpuThread, ctx));
}

bool probe_water(spu::Context& c) {
    if ((owner(c)->id >> 24) == 0x7d || !owner(c)->id) return false;
    if (be32(c.ls + 0x4000) == 0x43898202u && be32(c.ls + 0x4004) == 0x421b2282u) g_spu_water[be32(c.ls + 0x1dc) & 63].fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool probe_entry(spu::Context& c) {
    const uint8_t* ls = c.ls;
    if ((owner(c)->id >> 24) == 0x7d || !owner(c)->id) return false;
    if (std::memcmp(ls + 0x4000, vm::ptr(kEdgeEa), 0x30) != 0) return false;
    g_spu_edge[be32(ls + 0x1dc) & 63].fetch_add(1, std::memory_order_relaxed);
    if (!cfg().verify || (g_probe_seq.fetch_add(1, std::memory_order_relaxed) % cfg().verify)) return false;
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
    std::vector<std::pair<uint32_t, uint32_t>> rg = {{0x4000, 0x4000 + 0xc430}, {kCmdLs, kCmdLs + cmd.size}, {kApiLs, kApiLs + 0x18}, {kApiLs + 0x1c, kApiLs + 0x54}, {kApiLs + 0xb0, kApiLs + 0xb4}, {0x1c8, 0x1cc}};
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
    t_probe.rings = be32(cmd.raw + cmd.param_off) + 0x80;
    static const std::vector<int> fresh = parse_list(std::getenv("GOWA_JOBSYS_EDGE_VFRESH"));
    for (int m : fresh) fresh_check(c, j, cmd, spu_num, buf, static_cast<uint32_t>(m));
    return false;
}

bool probe_return(spu::Context&) {
    if (!t_probe.active) return false;
    t_probe.active = false;
    uint32_t bad = 0, first = 0, ringb = 0;
    for (auto& [a, b] : t_probe.sb.writes) {
        if (vm::ptr(a)[0] != b) {
            if (a - t_probe.rings < 0x200 && (a & 0x7f) - 0x18 < 4) {
                ringb++;
                continue;
            }
            if (!bad) first = a;
            bad++;
        }
    }
    if (ringb) g_st.xv_ringb.fetch_add(1);
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

bool (*g_prev_guard)(ppu::Context&) = nullptr;

struct CsrScope {
    unsigned saved;
    CsrScope() : saved(_mm_getcsr()) { _mm_setcsr((saved & ~0x6000u) | 0x6000u | 0x8040u); }
    ~CsrScope() { _mm_setcsr(saved); }
};

#ifdef JSEDGE_CORE
struct CoreConsumer final : jobsys::Consumer {
    const char* name() const override { return "edge-lists"; }
    bool has_work() override { return any_ready(); }
    bool try_run(jobsys::JobCtx&) override {
        CsrScope cs;
        return work_once();
    }
};
CoreConsumer g_core_consumer;
#endif

const uint32_t kEntrySig[16] = {0x24ffc0d0, 0x24ff80d1, 0x24ff40d2, 0x24ff00fe, 0x24004080, 0x24fe8081, 0x1ce80081, 0x1c000251,
                                0x01a00452, 0x238ba883, 0x40800005, 0x238ba985, 0x4236e802, 0x330000fe, 0x081f817e, 0x42659003};
const uint32_t kRetSig[8] = {0x40804000, 0x30826402, 0x40800103, 0x3082ce04, 0x21a00b00, 0x30873c00, 0x34000102, 0x21a00b83};

}

bool enabled() { return cfg().on || cfg().verify; }

void init() {
    if (!enabled()) return;
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;
    spu::hle::install_builtin();
    if (cfg().verify) spu::hle::register_function({0x3368, 8, kRetSig, probe_return, "EdgeConsumer.verify_ret"});
    spu::hle::register_function({kEntryLs, 16, kEntrySig, probe_entry, "EdgeConsumer.probe_entry"});
    spu::hle::register_function({0xb7c8, 0, nullptr, probe_water, "EdgeConsumer.probe_water", nullptr, 0xcbf29ce484222325ull});
    if (!cfg().on) {
        std::thread(manager_main).detach();
        return;
    }
    for (uint32_t i = 0; i < kVspus; i++) g_vspu[i].x = std::make_unique<Executor>(i);
    {
        uint32_t us = 100;
        if (const char* e = std::getenv("GOWA_JOBSYS_EDGE_COOL_US")) us = static_cast<uint32_t>(std::strtoul(e, nullptr, 0));
        auto c0 = std::chrono::steady_clock::now();
        uint64_t t0 = __rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        double hz = double(__rdtsc() - t0) / std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
        g_cool_cycles = static_cast<uint64_t>(hz * 1e-6 * us);
    }
    LOG_INFO("jsedge", "Edge list consumer on: %d workers, %u virtual SPUs, help %d, reference %d, verify %u", cfg().threads, cfg().vspus, cfg().help ? 1 : 0, cfg().reference ? 1 : 0, cfg().verify);
    std::thread(manager_main).detach();
    for (int k = 0; k < cfg().threads; k++) std::thread(worker_main, k).detach();
#ifdef JSEDGE_CORE
    if (jobsys::enabled()) jobsys::add_consumer(&g_core_consumer);
#endif
}

bool spu_idle(unsigned slot) {
    if (!cfg().on || !cfg().donate || !g_started.load(std::memory_order_relaxed)) return false;
    g_st.idle_calls.fetch_add(1, std::memory_order_relaxed);
    if (!any_ready()) return false;
    if (!work_once(slot < kVspus ? static_cast<int>(slot) : -1)) return false;
    g_st.donated.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool on_set_priorities(ppu::Context& ctx) {
    if (!cfg().on) return false;
    uint32_t obj = static_cast<uint32_t>(ctx.gpr[3]);
    if (!vm::range_mapped(obj, 12)) return false;
    uint32_t wid = vm::read32(obj + 8);
    if (wid < kFirstWid || wid >= kFirstWid + kLists) return false;
    List& L = g_lists[wid - kFirstWid];
    int s = L.state.load(std::memory_order_acquire);
    if (s != S_PENDING && s != S_OWNED) return false;
    uint32_t p = static_cast<uint32_t>(ctx.gpr[4]);
    if (!vm::range_mapped(p, 8)) return false;
    std::memcpy(L.prio_saved, vm::ptr(p), 8);
    g_st.prio_hooked.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void set_prev_guard(bool (*g)(ppu::Context&)) {
    if (g != on_wait_previous) g_prev_guard = g;
}

bool on_wait_previous(ppu::Context& ctx) {
    if (!cfg().on || !cfg().help) return g_prev_guard ? g_prev_guard(ctx) : false;
    uint32_t cur = vm::read32(0xa0c13c);
    if (cur > 2) return g_prev_guard ? g_prev_guard(ctx) : false;
    uint32_t prev = (cur + 2) % 3;
    if (!*vm::ptr(0xa0c12b + prev)) return g_prev_guard ? g_prev_guard(ctx) : false;
    uint32_t counter = 0xa0b740 + 4 * prev;
    if (!vm::read16(counter)) return g_prev_guard ? g_prev_guard(ctx) : false;
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
    return g_prev_guard ? g_prev_guard(ctx) : false;
}

}
