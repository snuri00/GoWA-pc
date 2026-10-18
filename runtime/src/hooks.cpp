#include "ppu/ops.h"
#include "runtime/hooks.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/trace.h"

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

void impl(ppu::Context& ctx, uint8_t* base, uint32_t addr, ppu::BodyFn fn) {
    auto found = g_hooks.find(addr);
    Hook* h = found != g_hooks.end() ? found->second : nullptr;
    if (!h) {
        fn(ctx, base);
        return;
    }
    h->calls.fetch_add(1, std::memory_order_relaxed);
    if (h->guard && h->guard(ctx)) return;
    if (h->panic) {
        record(h, ctx);
        std::string msg = guest_string(static_cast<uint32_t>(ctx.gpr[3]));
        LOG_WARN("blackbox", "game error call %s lr %08x: '%s' r4=%llx r5=%llx r6=%llx", name_of(addr).c_str(), static_cast<uint32_t>(ctx.lr), msg.c_str(), static_cast<unsigned long long>(ctx.gpr[4]),
                 static_cast<unsigned long long>(ctx.gpr[5]), static_cast<unsigned long long>(ctx.gpr[6]));
        guest_backtrace(ctx);
        if (g_panics.fetch_add(1) < 2) capture("game error");
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
    for (uint32_t i = 0; !vm::read16(state + 0x70); i++) {
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(200)) {
            LOG_WARN("patch", "edge list %u did not start within 200 ms", prev);
            return false;
        }
        if (i < 256) std::this_thread::yield();
        else std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    static std::atomic<uint32_t> logged{0};
    if (logged.fetch_add(1) < 8) {
        LOG_WARN("patch", "waited %.3f ms for edge list %u to pass its MeshPrep barrier", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(), prev);
    }
    return false;
}

void install_patches() {
    if (const char* e = std::getenv("GOWA_GAME_PATCHES"); e && std::string(e) == "0") return;
    struct Patch {
        uint32_t addr;
        bool (*guard)(ppu::Context&);
    };
    static const Patch patches[] = {{0x39ea58, skip_repeated_mip_eviction}, {0x41c480, wait_previous_edge_list_started}};
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

void init() {
    if (const char* n = std::getenv("GOWA_SYMBOLS")) load_names(n);
    else load_names("../docs/re/symbols.txt");
    std::string combined = std::getenv("GOWA_HOOK") ? std::getenv("GOWA_HOOK") : "";
    const char* bb = std::getenv("GOWA_BLACKBOX");
    if (!bb) bb = "52bd7c:panic,3a2adc:ring,3a2690:ring,3a12a0:ring,3a0000:ring:p9b26ac*=0x40,39f3c4:ring,39f06c:ring,39b6f4:ring:d3=0x40";
    if (*bb && std::string(bb) != "0") combined += (combined.empty() ? "" : ",") + std::string(bb);
    const char* spec = combined.c_str();
    if (!*spec) {
        install_patches();
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
    if (g_hooks.empty()) return;
    ppu::hook_impl = impl;
    if (g_count_only) std::thread(summary_thread).detach();
}

}
