#include "runtime/critpath.h"

#include "ppu/context.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/thread.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace critpath {

bool g_on = false;
std::atomic<uint32_t> g_watch_n{0};
uint32_t g_pm_sig[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};

namespace {

constexpr uint32_t RING = 1u << 15;

struct Ring {
    std::atomic<uint64_t> head{0};
    std::atomic<uint64_t> tail{0};
    std::atomic<uint64_t> dropped{0};
    uint32_t id = 0;
    bool main = false;
    char name[40] = {};
    Ev ev[RING];
};

std::mutex g_rings_lock;
std::vector<Ring*> g_rings;
thread_local Ring* t_ring = nullptr;
thread_local uint32_t t_depth = 0;
thread_local uint32_t t_sleeps = 0;
thread_local bool t_seg_open = false;
thread_local uint64_t t_seg_start = 0;
thread_local uint32_t t_seg_polls = 0;
double g_tsc_hz = 0;
uint32_t g_sys_min = 0;

struct Watch {
    std::atomic<uint32_t> word{0};
    uint32_t len = 16;
    uint8_t mode = 0;
};
constexpr uint32_t MAXW = 16;
Watch g_watch[MAXW];
std::mutex g_watch_lock;
uint32_t g_batch_watch = 0xFFFFFFFFu;

Ring* make_ring() {
    Ring* r = new Ring;
    std::string name;
    if (guest::Thread* t = guest::current()) name = t->name;
    if (name.empty()) {
        PWSTR w = nullptr;
        if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &w)) && w) {
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            name.resize(n > 0 ? n - 1 : 0);
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, name.data(), n, nullptr, nullptr);
            LocalFree(w);
        }
    }
    if (name.empty()) name = "tid" + std::to_string(GetCurrentThreadId());
    std::snprintf(r->name, sizeof r->name, "%s", name.c_str());
    r->main = name == "main_thread";
    std::lock_guard lk(g_rings_lock);
    r->id = static_cast<uint32_t>(g_rings.size());
    g_rings.push_back(r);
    return r;
}

Ring* ring() {
    if (!t_ring) t_ring = make_ring();
    return t_ring;
}

uint32_t add_watch(uint32_t word, uint32_t len, uint8_t mode) {
    std::lock_guard lk(g_watch_lock);
    for (uint32_t i = 0; i < MAXW; i++) {
        if (g_watch[i].word.load(std::memory_order_relaxed)) continue;
        g_watch[i].len = len;
        g_watch[i].mode = mode;
        g_watch[i].word.store(word, std::memory_order_release);
        if (g_watch_n.load() < i + 1) g_watch_n.store(i + 1);
        return i;
    }
    return 0xFFFFFFFFu;
}

void drop_watch(uint32_t i) {
    if (i >= MAXW) return;
    std::lock_guard lk(g_watch_lock);
    g_watch[i].word.store(0, std::memory_order_release);
}

uint16_t list_min(uint32_t at) {
    uint16_t m = 0xFFFF;
    for (uint32_t k = 2; k < 16; k += 2) {
        uint16_t v = vm::read16(at + k);
        if (v < m) m = v;
    }
    return m;
}

uint32_t batch_obj() {
    uint32_t obj = vm::is_mapped(0x858c2c) ? vm::read32(0x858c2c) : 0;
    if (!obj || !vm::range_mapped(obj, 0xe0)) return 0;
    return obj;
}

void ensure_batch_watch() {
    if (g_batch_watch != 0xFFFFFFFFu) return;
    uint32_t obj = batch_obj();
    if (!obj) return;
    uint32_t hdr = vm::read32(obj);
    if (!hdr || !vm::range_mapped(hdr, 0x80)) return;
    g_batch_watch = add_watch(hdr + 0x40, 64, 1);
}

uint64_t batch_mins(uint32_t hdr) {
    uint64_t f = 0;
    for (uint32_t i = 0; i < 4; i++) f |= uint64_t(list_min(hdr + 0x40 + i * 16)) << (16 * i);
    return f;
}

struct Agg {
    uint64_t frames = 0;
    uint64_t wait[16] = {};
    uint64_t sys = 0;
    uint64_t t_first = 0, t_last = 0;
    uint64_t win0 = 0;
};

FILE* g_out = nullptr;

void write_all(Agg& agg) {
    std::vector<Ring*> rings;
    {
        std::lock_guard lk(g_rings_lock);
        rings = g_rings;
    }
    static std::vector<bool> named;
    static std::vector<Ev> buf;
    for (Ring* r : rings) {
        if (named.size() <= r->id) named.resize(r->id + 1, false);
        if (!named[r->id]) {
            named[r->id] = true;
            uint32_t hdr[2] = {1, r->id};
            std::fwrite(hdr, 4, 2, g_out);
            std::fwrite(r->name, 1, sizeof r->name, g_out);
        }
        uint64_t h = r->head.load(std::memory_order_acquire), t = r->tail.load(std::memory_order_relaxed);
        if (h == t) continue;
        buf.resize(h - t);
        for (uint64_t i = t; i < h; i++) buf[i - t] = r->ev[i & (RING - 1)];
        r->tail.store(h, std::memory_order_release);
        uint32_t hdr[3] = {2, r->id, static_cast<uint32_t>(buf.size())};
        std::fwrite(hdr, 4, 3, g_out);
        std::fwrite(buf.data(), sizeof(Ev), buf.size(), g_out);
        if (!r->main) continue;
        for (const Ev& e : buf) {
            if (e.type == EV_FRAME) {
                agg.frames++;
                if (!agg.t_first) agg.t_first = e.t0;
                agg.t_last = e.t0;
            } else if (e.type == EV_WAIT && e.depth == 0 && e.kind < 16) {
                agg.wait[e.kind] += e.t1 - e.t0;
            } else if (e.type == EV_SYSCALL && e.depth == 0) {
                agg.sys += e.t1 - e.t0;
            }
        }
    }
}

const char* kind_name(int k) {
    static const char* names[] = {"none", "frame", "counter", "batchfin", "batchadd", "kick", "wws", "edgeprev", "graph", "graphc", "rsx", "push", "edgeguard", "k13", "k14", "k15"};
    return names[k & 15];
}

void writer() {
    SetThreadDescription(GetCurrentThread(), L"critpath writer");
    Agg agg;
    agg.win0 = __rdtsc();
    auto last_log = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        write_all(agg);
        auto now = std::chrono::steady_clock::now();
        if (now - last_log < std::chrono::seconds(5)) continue;
        last_log = now;
        std::fflush(g_out);
        uint64_t win1 = __rdtsc();
        if (agg.frames < 2) {
            agg = Agg{};
            agg.win0 = win1;
            continue;
        }
        double span = double(win1 - agg.win0);
        std::string s;
        char b[64];
        double total = 0;
        for (int k = 0; k < 16; k++) {
            if (!agg.wait[k]) continue;
            double p = 100.0 * double(agg.wait[k]) / span;
            total += p;
            std::snprintf(b, sizeof b, " %s %.1f", kind_name(k), p);
            s += b;
        }
        uint64_t dropped = 0;
        {
            std::lock_guard lk(g_rings_lock);
            for (Ring* r : g_rings) dropped += r->dropped.load();
        }
        LOG_INFO("critpath", "ppu frames %.1f/s, main waits %.1f%% of wall time:%s | main blocking syscalls outside waits %.1f%% | dropped %llu", double(agg.frames) * g_tsc_hz / span, total, s.c_str(),
                 100.0 * double(agg.sys) / span, static_cast<unsigned long long>(dropped));
        agg = Agg{};
        agg.win0 = win1;
    }
}

double calibrate() {
    LARGE_INTEGER f, q0, q1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q0);
    uint64_t t0 = __rdtsc();
    Sleep(60);
    QueryPerformanceCounter(&q1);
    uint64_t t1 = __rdtsc();
    return double(t1 - t0) * double(f.QuadPart) / double(q1.QuadPart - q0.QuadPart);
}

const Hook g_hooks[] = {
    {0x2d0258, K_FRAME},       {0x3d3794, K_COUNTER},  {0x49b85c, K_BATCH_FINISH}, {0x49b76c, K_BATCH_FINISH}, {0x49b4a8, K_BATCH_ADD},
    {0x49b210, K_BATCH_KICK},  {0x49d688, K_WWS_WAIT}, {0x41cbdc, K_EDGE_PREV},    {0x44ca48, K_GRAPH_POLL},   {0x44cd20, K_GRAPH},
    {0x44d120, K_GRAPH},       {0x3a3670, K_RSX},      {0x49dbac, K_WWS_PUSH},
};

bool is_wait(uint8_t k) { return k == K_COUNTER || k == K_BATCH_FINISH || k == K_BATCH_ADD || k == K_WWS_WAIT || k == K_EDGE_PREV || k == K_RSX; }

}

void init() {
    const char* e = std::getenv("GOWA_CRITPATH");
    if (!e || !*e || e[0] == '0') return;
    const char* path = std::getenv("GOWA_CRITPATH_FILE");
    g_out = std::fopen(path && *path ? path : "critpath.bin", "wb");
    if (!g_out) {
        LOG_WARN("critpath", "cannot open output file");
        return;
    }
    g_tsc_hz = calibrate();
    g_sys_min = static_cast<uint32_t>(g_tsc_hz * 20e-6);
    char magic[8] = {'C', 'P', 'A', 'T', 'H', '3', 0, 0};
    std::fwrite(magic, 1, 8, g_out);
    std::fwrite(&g_tsc_hz, 8, 1, g_out);
    g_on = true;
    std::thread(writer).detach();
    LOG_INFO("critpath", "critical-path recording on (tsc %.3f GHz)", g_tsc_hz / 1e9);
}

void push(const Ev& e) {
    Ring* r = ring();
    uint64_t h = r->head.load(std::memory_order_relaxed);
    if (h - r->tail.load(std::memory_order_acquire) >= RING) {
        r->dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    r->ev[h & (RING - 1)] = e;
    r->head.store(h + 1, std::memory_order_release);
}

const Hook* hook_table(int& n) {
    n = static_cast<int>(sizeof g_hooks / sizeof g_hooks[0]);
    return g_hooks;
}

void enter(uint8_t kind, ppu::Context& ctx, Call& c) {
    c.t0 = __rdtsc();
    c.sleeps = t_sleeps;
    switch (kind) {
    case K_FRAME: {
        static uint32_t frame = 0;
        Ev e{};
        e.type = EV_FRAME;
        e.t0 = e.t1 = c.t0;
        e.a = frame++;
        push(e);
        return;
    }
    case K_COUNTER: {
        static const uint32_t counter_watch = add_watch(0x9c7668, 4, 0);
        (void)counter_watch;
        c.a = 0x9c7668;
        c.b = vm::read32(0x9c7668);
        c.e = vm::read32(0x9c767c);
        break;
    }
    case K_BATCH_FINISH:
    case K_BATCH_ADD:
    case K_BATCH_KICK: {
        ensure_batch_watch();
        uint32_t obj = static_cast<uint32_t>(ctx.gpr[3]);
        c.a = obj;
        if (vm::range_mapped(obj, 0xe0)) {
            c.b = vm::read32(obj + 0xcc);
            c.c = vm::read32(obj + 0xd0);
            c.e = vm::read32(obj + 0xd8) | (vm::read32(obj + 0xdc) << 16);
        }
        break;
    }
    case K_WWS_WAIT: {
        uint32_t obj = static_cast<uint32_t>(ctx.gpr[3]);
        uint32_t idx = static_cast<uint32_t>(ctx.gpr[4]);
        c.a = obj;
        c.b = idx;
        uint32_t hdr = vm::range_mapped(obj, 4) ? vm::read32(obj) : 0;
        c.c = hdr;
        if (hdr && vm::range_mapped(hdr + 64 + idx * 16, 16)) {
            c.e = list_min(hdr + 64 + idx * 16);
            c.watch = add_watch(hdr + 64 + idx * 16, 16, 0);
        }
        break;
    }
    case K_WWS_PUSH:
        c.a = static_cast<uint32_t>(ctx.gpr[3]);
        c.b = static_cast<uint32_t>(ctx.gpr[4]);
        c.c = static_cast<uint32_t>(ctx.gpr[5]);
        break;
    case K_GRAPH_POLL:
        c.a = static_cast<uint32_t>(ctx.gpr[3]);
        return;
    default:
        break;
    }
    if (is_wait(kind)) t_depth++;
}

void leave(uint8_t kind, ppu::Context& ctx, Call& c) {
    uint64_t t1 = __rdtsc();
    if (kind == K_FRAME) return;
    if (kind == K_GRAPH_POLL) {
        if (static_cast<uint32_t>(ctx.gpr[3]) == 0) {
            if (!t_seg_open) {
                t_seg_open = true;
                t_seg_start = c.t0;
                t_seg_polls = 0;
            }
            t_seg_polls++;
            return;
        }
        if (t_seg_open) {
            t_seg_open = false;
            Ev e{};
            e.type = EV_WAIT;
            e.kind = K_GRAPH_POLL;
            e.depth = static_cast<uint8_t>(t_depth);
            e.t0 = t_seg_start;
            e.t1 = c.t0;
            e.a = c.a;
            e.b = t_seg_polls;
            push(e);
        }
        return;
    }
    if (kind == K_GRAPH) {
        if (t_seg_open) {
            t_seg_open = false;
            Ev e{};
            e.type = EV_WAIT;
            e.kind = K_GRAPH_POLL;
            e.depth = static_cast<uint8_t>(t_depth);
            e.t0 = t_seg_start;
            e.t1 = t1;
            e.a = static_cast<uint32_t>(ctx.gpr[31]);
            e.b = t_seg_polls;
            e.c = 1;
            push(e);
        }
        return;
    }
    if (is_wait(kind) && t_depth) t_depth--;
    Ev e{};
    e.t0 = c.t0;
    e.t1 = t1;
    e.kind = kind;
    e.depth = static_cast<uint8_t>(t_depth);
    e.a = c.a;
    e.b = c.b;
    e.c = c.c;
    e.d = static_cast<uint32_t>(ctx.lr);
    e.e = c.e;
    uint32_t sl = t_sleeps - c.sleeps;
    e.pad = static_cast<uint8_t>(sl > 255 ? 255 : sl);
    switch (kind) {
    case K_COUNTER:
        e.c = vm::read32(0x9c7668);
        break;
    case K_BATCH_FINISH:
    case K_BATCH_ADD:
    case K_BATCH_KICK:
        if (vm::range_mapped(c.a, 0xe0)) {
            e.c = vm::read32(c.a + 0xcc);
            e.d = vm::read32(c.a + 0xd0);
            uint32_t hdr = vm::read32(c.a);
            if (hdr && vm::range_mapped(hdr, 0x80)) e.f = batch_mins(hdr);
        }
        e.e = c.e | (c.c << 24);
        break;
    case K_WWS_WAIT:
        if (c.c && vm::range_mapped(c.c + 64 + c.b * 16, 16)) e.f = list_min(c.c + 64 + c.b * 16);
        drop_watch(c.watch);
        break;
    default:
        break;
    }
    e.type = kind == K_BATCH_KICK ? EV_KICK : kind == K_WWS_PUSH ? EV_PUSH : EV_WAIT;
    push(e);
}

void emit_wait(uint8_t kind, uint64_t t0, uint64_t t1, uint32_t a, uint32_t b, uint32_t c) {
    Ev e{};
    e.type = EV_WAIT;
    e.kind = kind;
    e.depth = static_cast<uint8_t>(t_depth);
    e.t0 = t0;
    e.t1 = t1;
    e.a = a;
    e.b = b;
    e.c = c;
    push(e);
}

uint32_t back_ra(uint32_t& sp) {
    if (!sp || (sp & 7) || !vm::range_mapped(sp, 8)) return sp = 0;
    uint32_t up = static_cast<uint32_t>(vm::read64(sp));
    if (!up || (up & 7) || up <= sp || !vm::range_mapped(up + 16, 8)) return sp = 0;
    sp = up;
    return static_cast<uint32_t>(vm::read64(up + 16));
}

void syscall_done(uint32_t n, uint64_t t0, uint32_t lr, uint32_t arg, uint32_t sp) {
    if (n == 141) t_sleeps++;
    uint64_t t1 = __rdtsc();
    if (t1 - t0 < g_sys_min) return;
    Ev e{};
    e.type = EV_SYSCALL;
    e.depth = static_cast<uint8_t>(t_depth + (t_seg_open ? 1 : 0));
    e.t0 = t0;
    e.t1 = t1;
    e.a = n;
    e.d = lr;
    e.b = back_ra(sp);
    e.c = back_ra(sp);
    e.e = back_ra(sp);
    e.f = arg | (uint64_t(back_ra(sp)) << 32);
    push(e);
}

void flip(uint64_t count) {
    Ev e{};
    e.type = EV_FLIP;
    e.t0 = e.t1 = __rdtsc();
    e.a = static_cast<uint32_t>(count);
    push(e);
}

void spu_init_sig() {
    if (g_pm_sig[0] != 0xFFFFFFFFu) return;
    if (!vm::range_mapped(0x7a7b00, 4) || !vm::range_mapped(0x7aab00, 4)) return;
    std::memcpy(&g_pm_sig[1], vm::g_base + 0x7aab00, 4);
    std::memcpy(&g_pm_sig[0], vm::g_base + 0x7a7b00, 4);
}

void spu_job_begin(SpuState& s, uint32_t slot, const uint8_t* ls, const uint32_t* line_src, uint32_t pc, bool wws) {
    (void)slot;
    s.open = true;
    s.wws = wws;
    s.start = __rdtsc();
    s.lr_wait = s.blk_wait = 0;
    s.lab_n = 0;
    s.emu = s.declined = 0;
    s.src = line_src[(wws ? 0x4000 : 0x3000) >> 7];
    (void)pc;
    uint32_t wid;
    std::memcpy(&wid, ls + 0x1dc, 4);
    s.wid = __builtin_bswap32(wid);
}

void spu_job_end(SpuState& s, uint32_t slot) {
    uint64_t t1 = __rdtsc();
    s.open = false;
    s.last_src = s.src;
    s.last_wid = s.wid;
    auto sat = [](uint64_t v, uint64_t m) { return static_cast<uint32_t>(v > m ? m : v); };
    Ev e{};
    e.type = EV_JOB;
    e.t0 = s.start;
    e.t1 = t1;
    e.a = s.src;
    e.b = (s.wid & 0xFF) | (sat(s.lab_n, 0xFF) << 8) | (sat(s.emu >> 4, 0xFFFF) << 16);
    e.pad = static_cast<uint8_t>(sat(s.declined, 0xFF));
    e.c = (slot & 0xFF) | (uint32_t(s.wws) << 8) | (sat((s.idle_wait + s.idle_blk) >> 8, 0x7FFFFF) << 9);
    uint64_t q = s.pick && s.pick <= s.start ? s.start - s.pick : 0;
    e.d = sat(q, 0xFFFFFFFFu);
    e.e = s.lab_n > 1 ? sat(s.lab_last - s.lab_first, 0xFFFFFFFFu) : 0;
    e.f = sat(s.lr_wait, 0xFFFFFFFFu) | (uint64_t(sat(s.blk_wait, 0xFFFFFFFFu)) << 32);
    s.idle_wait = s.idle_blk = 0;
    push(e);
}

void spu_store(const SpuState& s, uint32_t slot, uint32_t ea, uint32_t size) {
    uint32_t n = g_watch_n.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < n && i < MAXW; i++) {
        uint32_t w = g_watch[i].word.load(std::memory_order_acquire);
        if (!w) continue;
        uint32_t len = g_watch[i].len;
        if (ea >= w + len || ea + size <= w) continue;
        Ev e{};
        e.type = EV_STORE;
        e.kind = static_cast<uint8_t>(i);
        e.depth = s.open ? 1 : 0;
        e.t0 = e.t1 = __rdtsc();
        e.a = w;
        e.b = (size & 0xFFFF) | (slot << 16) | ((s.open ? s.wid : s.last_wid) << 24);
        e.e = s.open ? s.src : s.last_src;
        if (g_watch[i].mode == 1) {
            e.c = vm::read32(w);
            e.f = batch_mins(w - 0x40);
        } else {
            e.c = vm::read32(w);
            if (len >= 16) {
                e.d = vm::read32(w + 4);
                e.f = vm::read64(w + 8);
            }
        }
        push(e);
    }
}

}
