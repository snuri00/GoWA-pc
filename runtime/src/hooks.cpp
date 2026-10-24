#include "ppu/ops.h"
#include "runtime/critpath.h"
#include "runtime/hooks.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/ppujobs.h"
#include "runtime/reservation.h"
#include "runtime/trace.h"
#include "jobsys/edge_consumer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <filesystem>

namespace spu {
void status_dump();
std::string brief();
}

namespace lv2 {
void rsx_status();
}

namespace guest {
void status_dump();
}

namespace hooks {

namespace {

struct Dump {
    int reg;
    uint32_t len;
    uint32_t ptr = 0;
    int derefs = 1;
};

struct Hook {
    uint32_t addr = 0;
    bool ring = false;
    bool panic = false;
    uint8_t cp = 0;
    bool quiet = false;
    bool (*guard)(ppu::Context&) = nullptr;
    std::vector<Dump> dumps;
    std::atomic<uint64_t> calls{0};
    std::atomic<uint32_t> logged{0};
};

std::unordered_map<uint32_t, Hook*> g_hooks;
std::unordered_map<uint32_t, std::string> g_names;
uint32_t g_max = 40;
double g_at = 0.0;
bool g_count_only = false;
thread_local int t_depth = 0;

struct RingEntry {
    double t;
    uint32_t addr;
    uint32_t lr;
    uint32_t tid;
    uint32_t dump_ea;
    uint64_t r[6];
    uint32_t data[16];
};

constexpr uint32_t RING_SIZE = 8192;
RingEntry g_ring[RING_SIZE];
std::atomic<uint64_t> g_ring_pos{0};
std::atomic<int> g_panics{0};
std::mutex g_capture_lock;

uint32_t dump_target(const Dump& d, const ppu::Context& ctx) {
    if (d.reg >= 0) return static_cast<uint32_t>(ctx.gpr[d.reg]);
    uint32_t ea = d.ptr;
    for (int k = 0; k < d.derefs && ea; k++) ea = vm::is_mapped(ea) ? vm::read32(ea) : 0;
    return ea;
}

void record(const Hook* h, const ppu::Context& ctx) {
    uint64_t i = g_ring_pos.fetch_add(1, std::memory_order_relaxed);
    RingEntry& e = g_ring[i % RING_SIZE];
    e.t = logging::uptime();
    e.addr = h->addr;
    e.lr = static_cast<uint32_t>(ctx.lr);
    e.tid = GetCurrentThreadId();
    for (int r = 0; r < 6; r++) e.r[r] = ctx.gpr[3 + r];
    e.dump_ea = 0;
    if (!h->dumps.empty()) {
        uint32_t ea = dump_target(h->dumps[0], ctx);
        if (ea && vm::range_mapped(ea, 64)) {
            e.dump_ea = ea;
            for (int k = 0; k < 16; k++) e.data[k] = vm::read32(ea + k * 4);
        }
    }
}

std::string guest_string(uint32_t ea) {
    std::string out;
    while (ea && out.size() < 200 && vm::is_mapped(ea)) {
        char ch = static_cast<char>(vm::g_base[ea++]);
        if (!ch) break;
        out += ch;
    }
    return out;
}

void guest_backtrace(const ppu::Context& ctx) {
    std::string bt;
    char b[32];
    std::snprintf(b, sizeof b, " lr %08x", static_cast<uint32_t>(ctx.lr));
    bt += b;
    uint32_t sp = static_cast<uint32_t>(ctx.gpr[1]);
    for (int i = 0; i < 32 && sp && vm::range_mapped(sp, 24); i++) {
        uint32_t next = static_cast<uint32_t>(vm::read64(sp));
        if (!next || next <= sp || !vm::range_mapped(next, 24)) break;
        uint32_t lr = static_cast<uint32_t>(vm::read64(next + 16));
        std::snprintf(b, sizeof b, " %08x", lr);
        bt += b;
        sp = next;
    }
    LOG_INFO("blackbox", "guest backtrace:%s", bt.c_str());
}

std::string thread_name() {
    PWSTR w = nullptr;
    std::string out;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &w)) && w) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        out.resize(n > 0 ? n - 1 : 0);
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
        LocalFree(w);
    }
    return out;
}

std::string hex_dump(uint32_t ea, uint32_t len) {
    std::string s;
    char b[12];
    for (uint32_t i = 0; i < len; i += 4) {
        if (!vm::is_mapped(ea + i)) {
            s += " ??";
            break;
        }
        std::snprintf(b, sizeof b, " %08x", vm::read32(ea + i));
        s += b;
    }
    return s;
}

void load_names(const char* path) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string a, n;
        if (!(ss >> a >> n) || a.empty() || a[0] == '#') continue;
        g_names[static_cast<uint32_t>(std::strtoul(a.c_str(), nullptr, 16))] = n;
    }
}

bool benign_game_error(const std::string& msg) { return msg.rfind("BATCHJOB: AddJob: waiting for room", 0) == 0; }

void impl_body(Hook* h, ppu::Context& ctx, uint8_t* base, uint32_t addr, ppu::BodyFn fn);

void impl(ppu::Context& ctx, uint8_t* base, uint32_t addr, ppu::BodyFn fn) {
    auto found = g_hooks.find(addr);
    Hook* h = found != g_hooks.end() ? found->second : nullptr;
    if (!h) {
        fn(ctx, base);
        return;
    }
    if (h->cp) {
        critpath::Call c;
        critpath::enter(h->cp, ctx, c);
        impl_body(h, ctx, base, addr, fn);
        critpath::leave(h->cp, ctx, c);
        return;
    }
    impl_body(h, ctx, base, addr, fn);
}

void impl_body(Hook* h, ppu::Context& ctx, uint8_t* base, uint32_t addr, ppu::BodyFn fn) {
    h->calls.fetch_add(1, std::memory_order_relaxed);
    if (h->guard && h->guard(ctx)) return;
    if (h->quiet) {
        fn(ctx, base);
        return;
    }
    if (h->panic) {
        record(h, ctx);
        std::string msg = guest_string(static_cast<uint32_t>(ctx.gpr[3]));
        LOG_WARN("blackbox", "game error call %s lr %08x: '%s' r4=%llx r5=%llx r6=%llx", name_of(addr).c_str(), static_cast<uint32_t>(ctx.lr), msg.c_str(), static_cast<unsigned long long>(ctx.gpr[4]),
                 static_cast<unsigned long long>(ctx.gpr[5]), static_cast<unsigned long long>(ctx.gpr[6]));
        guest_backtrace(ctx);
        if (!benign_game_error(msg) && g_panics.fetch_add(1) < 2) capture("game error");
        fn(ctx, base);
        return;
    }
    if (h->ring) {
        record(h, ctx);
        fn(ctx, base);
        return;
    }
    bool log = !g_count_only && logging::uptime() >= g_at && h->logged.load(std::memory_order_relaxed) < g_max;
    if (!log) {
        fn(ctx, base);
        return;
    }
    uint32_t n = h->logged.fetch_add(1, std::memory_order_relaxed);
    const std::string& name = name_of(addr);
    std::string args;
    char b[64];
    for (int r = 3; r <= 8; r++) {
        std::snprintf(b, sizeof b, " r%d=%llx", r, static_cast<unsigned long long>(ctx.gpr[r]));
        args += b;
    }
    std::snprintf(b, sizeof b, " f1=%g f2=%g", ctx.fpr[1], ctx.fpr[2]);
    args += b;
    LOG_INFO("hook", "%*s-> %s #%u lr %08x [%s]%s", t_depth * 2, "", name.c_str(), n, static_cast<uint32_t>(ctx.lr), thread_name().c_str(), args.c_str());
    for (const Dump& d : h->dumps) {
        if (d.reg < 0) {
            uint32_t ea = d.ptr;
            for (int k = 0; k < d.derefs && ea; k++) ea = vm::is_mapped(ea) ? vm::read32(ea) : 0;
            LOG_INFO("hook", "%*s   *%08x@%08x:%s", t_depth * 2, "", d.ptr, ea, ea && vm::range_mapped(ea, d.len) ? hex_dump(ea, d.len).c_str() : " unmapped");
            continue;
        }
        uint32_t ea = static_cast<uint32_t>(ctx.gpr[d.reg]);
        LOG_INFO("hook", "%*s   r%d@%08x:%s", t_depth * 2, "", d.reg, ea, hex_dump(ea, d.len).c_str());
    }
    uint64_t r1 = ctx.gpr[1];
    t_depth++;
    fn(ctx, base);
    t_depth--;
    LOG_INFO("hook", "%*s<- %s #%u r3=%llx f1=%g%s", t_depth * 2, "", name.c_str(), n, static_cast<unsigned long long>(ctx.gpr[3]), ctx.fpr[1], ctx.gpr[1] != r1 ? " (stack mismatch)" : "");
}

std::string batch_state(uint32_t obj) {
    std::string s;
    char b[160];
    std::snprintf(b, sizeof b, "added %u done %u cur %u lists %u mode %u per %u", vm::read32(obj + 0xcc), vm::read32(obj + 0xd0), vm::read32(obj + 0xd4), vm::read32(obj + 0xd8), vm::read32(obj + 0xdc), vm::read32(obj + 0xc8));
    s += b;
    uint32_t hdr = vm::read32(obj);
    if (hdr && vm::range_mapped(hdr, 0x80)) {
        for (uint32_t i = 0; i < 4; i++) {
            uint32_t st = hdr + 0x40 + i * 0x10;
            std::snprintf(b, sizeof b, " | L%u n%u st", i, vm::read32(obj + 0x2c + i * 0x14));
            s += b;
            for (uint32_t k = 0; k < 8; k++) {
                std::snprintf(b, sizeof b, " %04x", vm::read16(st + k * 2));
                s += b;
            }
        }
        std::snprintf(b, sizeof b, " | hdr %08x %08x %08x %08x", vm::read32(hdr), vm::read32(hdr + 4), vm::read32(hdr + 8), vm::read32(hdr + 12));
        s += b;
    }
    const uint32_t spurs = 0x3001ad00;
    uint32_t wid = vm::read32(obj + 8) & 31;
    std::snprintf(b, sizeof b, " | wid %u ready %u cont %02x pend %02x idling %02x ssm %02x sig %04x flagrecv %02x", wid, vm::g_base[spurs + (wid & 15) + (wid < 16 ? 0 : 0x10)], vm::g_base[spurs + 0x20 + (wid & 15)],
                  vm::g_base[spurs + 0x30 + (wid & 15)], vm::g_base[spurs + 0x73], vm::g_base[spurs + 0x72], vm::read16(spurs + 0x70), vm::g_base[spurs + 0x77]);
    s += b;
    return s;
}

void batch_diag_thread() {
    using clk = std::chrono::steady_clock;
    std::vector<uint8_t> last;
    auto since = clk::now();
    auto wait_since = clk::now();
    int wait_kind = 0;
    bool reported = false, wait_reported = false;
    int samples = 0;
    double thresh = 20.0;
    if (const char* e = std::getenv("GOWA_BATCH_DIAG")) thresh = (std::max)(1.0, std::atof(e));
    for (;;) {
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        uint32_t obj = vm::is_mapped(0x858c2c) ? vm::read32(0x858c2c) : 0;
        if (!obj || !vm::range_mapped(obj, 0xe0)) continue;
        uint32_t hdr = vm::read32(obj);
        if (!hdr || !vm::range_mapped(hdr, 0x80)) continue;
        uint32_t added = vm::read32(obj + 0xcc), done = vm::read32(obj + 0xd0), mode = vm::read32(obj + 0xdc);
        uint32_t per = vm::read32(obj + 0xc8), lists = vm::read32(obj + 0xd8);
        int kind = 0;
        if (mode == 2 && added != done) kind = 2;
        else if (mode == 1 && per && int32_t((lists + 4) * per - added) <= 0) kind = 1;
        auto now = clk::now();
        if (kind != wait_kind) {
            double ms = std::chrono::duration<double, std::milli>(now - wait_since).count();
            if (wait_kind && ms > thresh) LOG_WARN("batchdiag", "ppu %s wait ended after %.1f ms: %s", wait_kind == 2 ? "finish" : "addjob", ms, batch_state(obj).c_str());
            wait_kind = kind;
            wait_since = now;
            wait_reported = false;
            samples = 0;
        } else if (kind) {
            double ms = std::chrono::duration<double, std::milli>(now - wait_since).count();
            if (ms > thresh && samples < 8 && ms > thresh * (1 << samples)) {
                samples++;
                LOG_WARN("batchdiag", "ppu %s wait %.1f ms: %s |%s", kind == 2 ? "finish" : "addjob", ms, batch_state(obj).c_str(), spu::brief().c_str());
                if (!wait_reported) {
                    wait_reported = true;
                    static std::atomic<int> dumps{0};
                    if (dumps.fetch_add(1) < 4) spu::status_dump();
                }
            }
        }
        std::vector<uint8_t> sig(vm::g_base + hdr + 0x40, vm::g_base + hdr + 0x80);
        sig.push_back(uint8_t(added));
        sig.push_back(uint8_t(done));
        bool pending = added != done;
        if (!pending || sig != last) {
            if (reported) LOG_WARN("batchdiag", "resolved after %.1f ms: %s", std::chrono::duration<double, std::milli>(now - since).count(), batch_state(obj).c_str());
            reported = false;
            last = sig;
            since = now;
            continue;
        }
        double ms = std::chrono::duration<double, std::milli>(now - since).count();
        if (!reported && ms > thresh) {
            reported = true;
            LOG_WARN("batchdiag", "no progress for %.1f ms (mode %u): %s |%s", ms, mode, batch_state(obj).c_str(), spu::brief().c_str());
        }
    }
}

void summary_thread() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::string s;
        for (auto& [a, h] : g_hooks) {
            char b[96];
            std::snprintf(b, sizeof b, " %s=%llu", name_of(a).c_str(), static_cast<unsigned long long>(h->calls.load()));
            s += b;
        }
        LOG_INFO("hook", "calls:%s", s.c_str());
    }
}

}

const std::string& name_of(uint32_t addr) {
    static std::mutex m;
    static std::unordered_map<uint32_t, std::string> cache;
    std::lock_guard lk(m);
    auto it = g_names.find(addr);
    if (it != g_names.end()) return it->second;
    auto& c = cache[addr];
    if (c.empty()) {
        char b[16];
        std::snprintf(b, sizeof b, "f_%08x", addr);
        c = b;
    }
    return c;
}

void capture(const char* reason) {
    std::lock_guard lk(g_capture_lock);
    static int n = 0;
    LOG_WARN("blackbox", "capture #%d: %s", n, reason);
    uint64_t end = g_ring_pos.load();
    uint64_t start = end > RING_SIZE ? end - RING_SIZE : 0;
    for (uint64_t i = start; i < end; i++) {
        const RingEntry& e = g_ring[i % RING_SIZE];
        std::string data;
        if (e.dump_ea) {
            char b[12];
            data = " @";
            std::snprintf(b, sizeof b, "%08x:", e.dump_ea);
            data += b;
            for (int k = 0; k < 16; k++) {
                std::snprintf(b, sizeof b, " %08x", e.data[k]);
                data += b;
            }
        }
        LOG_INFO("blackbox", "ring %.6f %s lr %08x tid %u r3=%llx r4=%llx r5=%llx r6=%llx r7=%llx r8=%llx%s", e.t, name_of(e.addr).c_str(), e.lr, e.tid, static_cast<unsigned long long>(e.r[0]),
                 static_cast<unsigned long long>(e.r[1]), static_cast<unsigned long long>(e.r[2]), static_cast<unsigned long long>(e.r[3]), static_cast<unsigned long long>(e.r[4]),
                 static_cast<unsigned long long>(e.r[5]), data.c_str());
    }
    spu::status_dump();
    guest::status_dump();
    lv2::rsx_status();
    static const bool no_mem = std::getenv("GOWA_BLACKBOX_NOMEM") != nullptr;
    if (!no_mem && n < 2) {
        std::filesystem::create_directories("blackbox");
        char path[64];
        std::snprintf(path, sizeof path, "blackbox/memory_%d.bin", n);
        trace::dump_memory(path);
    }
    n++;
    LOG_WARN("blackbox", "capture done");
}

bool skip_repeated_mip_eviction(ppu::Context& ctx) {
    uint32_t slot = static_cast<uint32_t>(ctx.gpr[4]);
    if (!vm::range_mapped(slot, 0x48)) return false;
    uint32_t record = vm::read32(slot + 0x44);
    if (!record || !vm::range_mapped(record, 0x20)) return false;
    uint32_t evict = vm::read32(slot + 0x1c);
    uint32_t size = vm::read32(record + 0xc);
    if (size >= evict) return false;
    static std::atomic<uint32_t> logged{0};
    if (logged.fetch_add(1) < 8) LOG_WARN("patch", "skipped repeated top-mip eviction of record 0x%08x (size 0x%x, evict 0x%x)", record, size, evict);
    return true;
}

bool wait_previous_edge_list_started(ppu::Context& ctx) {
    uint32_t cur = vm::read32(0xa0c13c);
    if (cur > 2) return false;
    uint32_t prev = (cur + 2) % 3;
    if (!*vm::ptr(0xa0c12b + prev)) return false;
    uint32_t list = vm::read32(0xa0c11c + prev * 4);
    if (!list || !vm::range_mapped(list, 0x30)) return false;
    uint32_t state = vm::read32(list);
    if (!state || !vm::range_mapped(state + 0x70, 2)) return false;
    if (vm::read16(state + 0x70)) return false;
    auto start = std::chrono::steady_clock::now();
    uint64_t cp0 = critpath::g_on ? __rdtsc() : 0;
    for (uint32_t i = 0; !vm::read16(state + 0x70); i++) {
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(200)) {
            LOG_WARN("patch", "edge list %u did not start within 200 ms", prev);
            return false;
        }
        if (i < 256) std::this_thread::yield();
        else std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    if (cp0) critpath::emit_wait(critpath::K_EDGE_GUARD, cp0, __rdtsc(), prev, list, 0);
    static std::atomic<uint32_t> logged{0};
    if (logged.fetch_add(1) < 8) {
        LOG_WARN("patch", "waited %.3f ms for edge list %u to pass its MeshPrep barrier", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(), prev);
    }
    return false;
}

bool restore_post_pass_keys(ppu::Context&) {
    if (const char* e = std::getenv("GOWA_POST_KEYS"); e && std::string(e) == "0") return false;
    uint64_t base = vm::read32(0x83d004) ? 0 : vm::read64(0x397400);
    struct Slot {
        uint32_t addr;
        uint64_t pass;
    };
    static const Slot slots[] = {{0x9abfb8, 4}, {0x9abfa0, 6}};
    static const bool blur = [] {
        const char* e = std::getenv("GOWA_MOTION_BLUR");
        return !(e && std::string(e) == "0");
    }();
    for (const Slot& s : slots) {
        if (s.pass == 4 && !blur) continue;
        if (vm::read64(s.addr)) continue;
        uint64_t key = (base & 0x0c3ffffffffffff0ull) | 0x9000000000000000ull | (uint64_t(5) << 54) | s.pass;
        vm::write64(s.addr, key);
        LOG_INFO("patch", "render pass key 0x%08x = 0x%016llx", s.addr, static_cast<unsigned long long>(key));
    }
    return false;
}

void tick_patches() {
    static const bool off = [] {
        const char* e = std::getenv("GOWA_GAME_PATCHES");
        return e && std::string(e) == "0";
    }();
    struct Boost {
        uint32_t wid;
        uint8_t prio[8];
        bool done;
    };
    static std::vector<Boost> boosts = [] {
        std::vector<Boost> v;
        std::string spec = std::getenv("GOWA_SPURS_PRIO") ? std::getenv("GOWA_SPURS_PRIO") : "";
        size_t pos = 0;
        while (pos < spec.size()) {
            size_t end = spec.find(';', pos);
            if (end == std::string::npos) end = spec.size();
            std::string item = spec.substr(pos, end - pos);
            size_t colon = item.find(':');
            if (colon != std::string::npos) {
                Boost b{static_cast<uint32_t>(std::atoi(item.substr(0, colon).c_str())), {}, false};
                std::string rest = item.substr(colon + 1);
                size_t q = 0;
                for (int i = 0; i < 8 && q <= rest.size(); i++) {
                    size_t c = rest.find(',', q);
                    if (c == std::string::npos) c = rest.size();
                    b.prio[i] = static_cast<uint8_t>(std::atoi(rest.substr(q, c - q).c_str()));
                    q = c + 1;
                }
                v.push_back(b);
            }
            pos = end + 1;
        }
        return v;
    }();
    if (off) return;
    const uint32_t spurs = 0x3001ad00;
    if (!vm::range_mapped(spurs, 0x1100)) return;
    bool changed = false;
    for (Boost& b : boosts) {
        if (b.done || b.wid >= 32) continue;
        if ((vm::read32(spurs + 0xB0) & (0x80000000u >> b.wid)) == 0) continue;
        uint32_t info = spurs + (b.wid < 16 ? 0xB00 : 0x1000) + (b.wid & 15) * 0x20;
        if (!vm::read32(info + 4)) continue;
        uint64_t prio = vm::read64(info + 0x18);
        if (!prio) continue;
        uint8_t old[8];
        for (int i = 0; i < 8; i++) old[i] = uint8_t(prio >> (56 - 8 * i));
        bool ok = true;
        for (int i = 0; i < 8; i++) {
            if ((old[i] == 0) != (b.prio[i] == 0)) ok = false;
        }
        b.done = true;
        if (!ok) {
            LOG_WARN("patch", "SPURS workload %u priority layout differs, not changed", b.wid);
            continue;
        }
        rsv::store_locked(info + 0x18, b.prio, 8);
        changed = true;
        LOG_WARN("patch", "SPURS workload %u priority %u,%u,%u,%u,%u,%u -> %u,%u,%u,%u,%u,%u", b.wid, old[0], old[1], old[2], old[3], old[4], old[5], b.prio[0], b.prio[1], b.prio[2], b.prio[3], b.prio[4], b.prio[5]);
    }
    if (changed) {
        uint8_t ff = 0xFF;
        rsv::store_locked(spurs + 0xBD, &ff, 1);
        rsv::store_locked(spurs + 0x72, &ff, 1);
    }
}

void install_critpath() {
    if (!critpath::g_on) return;
    int n = 0;
    const critpath::Hook* t = critpath::hook_table(n);
    for (int i = 0; i < n; i++) {
        Hook*& h = g_hooks[t[i].addr];
        if (!h) {
            h = new Hook;
            h->addr = t[i].addr;
            h->quiet = true;
        }
        h->cp = t[i].kind;
        ppu::hook_flags[t[i].addr >> 2] = 1;
    }
}

void install_patches() {
    if (const char* e = std::getenv("GOWA_GAME_PATCHES"); e && std::string(e) == "0") return;
    struct Patch {
        uint32_t addr;
        bool (*guard)(ppu::Context&);
    };
    static const Patch patches[] = {{0x39ea58, skip_repeated_mip_eviction}, {0x41c480, wait_previous_edge_list_started}, {0x2d0258, fps_unlock_timestep}, {0x397540, restore_post_pass_keys}};
    for (const Patch& p : patches) {
        Hook*& h = g_hooks[p.addr];
        if (!h) {
            h = new Hook;
            h->addr = p.addr;
            h->ring = true;
        }
        h->guard = p.guard;
        ppu::hook_flags[p.addr >> 2] = 1;
    }
}

void install_ppujobs() {
    if (!ppujobs::enabled()) return;
    struct Patch {
        uint32_t addr;
        bool (*guard)(ppu::Context&);
    };
    static const Patch patches[] = {{0x3e13e0, ppujobs::on_submit}, {0x49e540, ppujobs::on_push}, {0x49ce50, ppujobs::on_kick}, {0x49e368, ppujobs::on_wait}};
    for (const Patch& p : patches) {
        Hook*& h = g_hooks[p.addr];
        if (!h) {
            h = new Hook;
            h->addr = p.addr;
            h->quiet = true;
        }
        h->guard = p.guard;
        ppu::hook_flags[p.addr >> 2] = 1;
    }
}

void install_jobsys_edge() {
    if (!jsedge::enabled()) return;
    jsedge::init();
    Hook*& h = g_hooks[0x41cbdc];
    if (!h) {
        h = new Hook;
        h->addr = 0x41cbdc;
        h->quiet = true;
    }
    h->guard = jsedge::on_wait_previous;
    ppu::hook_flags[0x41cbdc >> 2] = 1;
}

void hog_thread(int lp, int on_ms, int off_ms, double from) {
    SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << lp);
    SetThreadDescription(GetCurrentThread(), L"test hog");
    while (logging::uptime() < from) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    LOG_WARN("hog", "normal-priority hog on lp %d (%d ms on, %d ms off)", lp, on_ms, off_ms);
    volatile uint64_t x = 0;
    for (;;) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(on_ms);
        while (std::chrono::steady_clock::now() < end) {
            for (int i = 0; i < 1000; i++) x = x + 1;
        }
        if (off_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(off_ms));
    }
}

void start_hogs(const char* spec) {
    double from = std::getenv("GOWA_TEST_HOG_FROM") ? std::atof(std::getenv("GOWA_TEST_HOG_FROM")) : 90.0;
    std::string s = spec;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(';', pos);
        if (end == std::string::npos) end = s.size();
        int lp = -1, on = 1000, off = 0;
        std::sscanf(s.substr(pos, end - pos).c_str(), "%d:%d:%d", &lp, &on, &off);
        if (lp >= 0 && lp < 64) std::thread(hog_thread, lp, on, off, from).detach();
        pos = end + 1;
    }
}

void init() {
    if (std::getenv("GOWA_BATCH_DIAG")) std::thread(batch_diag_thread).detach();
    if (const char* h = std::getenv("GOWA_TEST_HOG")) start_hogs(h);
    if (const char* n = std::getenv("GOWA_SYMBOLS")) load_names(n);
    else load_names("../docs/re/symbols.txt");
    std::string combined = std::getenv("GOWA_HOOK") ? std::getenv("GOWA_HOOK") : "";
    const char* bb = std::getenv("GOWA_BLACKBOX");
    if (!bb) bb = "52bd7c:panic,3a2adc:ring,3a2690:ring,3a12a0:ring,3a0000:ring:p9b26ac*=0x40,39f3c4:ring,39f06c:ring,39b6f4:ring:d3=0x40";
    if (*bb && std::string(bb) != "0") combined += (combined.empty() ? "" : ",") + std::string(bb);
    const char* spec = combined.c_str();
    if (!*spec) {
        install_patches();
        install_ppujobs();
        install_jobsys_edge();
        install_critpath();
        if (!g_hooks.empty()) ppu::hook_impl = impl;
        return;
    }
    if (const char* m = std::getenv("GOWA_HOOK_MAX")) g_max = static_cast<uint32_t>(std::atoi(m));
    if (const char* a = std::getenv("GOWA_HOOK_AT")) g_at = std::atof(a);
    g_count_only = std::getenv("GOWA_HOOK_COUNT") != nullptr;
    std::string all(spec);
    size_t pos = 0;
    while (pos <= all.size()) {
        size_t comma = all.find(',', pos);
        std::string item = all.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? all.size() + 1 : comma + 1;
        if (item.empty()) continue;
        std::vector<std::string> parts;
        size_t p = 0;
        while (true) {
            size_t c = item.find(':', p);
            parts.push_back(item.substr(p, c == std::string::npos ? std::string::npos : c - p));
            if (c == std::string::npos) break;
            p = c + 1;
        }
        uint32_t addr = 0;
        for (auto& [a, nm] : g_names) {
            if (nm == parts[0]) addr = a;
        }
        if (!addr) addr = static_cast<uint32_t>(std::strtoul(parts[0].c_str() + (parts[0].rfind("f_", 0) == 0 ? 2 : 0), nullptr, 16));
        if (!addr || (addr >> 2) >= sizeof(ppu::hook_flags)) {
            LOG_WARN("hook", "bad hook '%s'", item.c_str());
            continue;
        }
        if (g_hooks.count(addr)) continue;
        Hook* h = new Hook;
        h->addr = addr;
        for (size_t i = 1; i < parts.size(); i++) {
            if (parts[i] == "ring") h->ring = true;
            else if (parts[i] == "panic") h->panic = true;
            else if (parts[i].size() > 2 && parts[i][0] == 'd') {
                size_t eq = parts[i].find('=');
                int reg = std::atoi(parts[i].substr(1, eq - 1).c_str());
                uint32_t len = eq == std::string::npos ? 0x40 : static_cast<uint32_t>(std::strtoul(parts[i].c_str() + eq + 1, nullptr, 0));
                if (reg >= 0 && reg < 32) h->dumps.push_back({reg, len});
            } else if (parts[i].size() > 2 && parts[i][0] == 'p') {
                size_t eq = parts[i].find('=');
                std::string a = parts[i].substr(1, eq == std::string::npos ? std::string::npos : eq - 1);
                int derefs = 1;
                while (!a.empty() && a.back() == '*') {
                    derefs++;
                    a.pop_back();
                }
                uint32_t ptr = static_cast<uint32_t>(std::strtoul(a.c_str(), nullptr, 16));
                uint32_t len = eq == std::string::npos ? 0x40 : static_cast<uint32_t>(std::strtoul(parts[i].c_str() + eq + 1, nullptr, 0));
                h->dumps.push_back({-1, len, ptr, derefs});
            }
        }
        g_hooks[addr] = h;
        ppu::hook_flags[addr >> 2] = 1;
        if (!h->ring && !h->panic) LOG_INFO("hook", "hooked %s at 0x%08x", name_of(addr).c_str(), addr);
    }
    install_patches();
    install_ppujobs();
    install_jobsys_edge();
    install_critpath();
    if (g_hooks.empty()) return;
    ppu::hook_impl = impl;
    if (g_count_only) std::thread(summary_thread).detach();
}

}
