#include "runtime/trace.h"
#include "runtime/log.h"
#include "runtime/memory.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace trace {

uint32_t g_mask = 0;

namespace {

struct Rec {
    uint64_t t;
    uint16_t type;
    uint16_t tid;
    uint32_t x;
    uint64_t a, b, c, d, e;
};
static_assert(sizeof(Rec) == 56);

constexpr uint32_t kCap = 8192;

struct Buffer {
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    uint32_t n = 0;
    uint16_t tid = 0;
    Rec recs[kCap];
};

std::mutex g_io;
FILE* g_file = nullptr;
FILE* g_strings = nullptr;
std::mutex g_reg_lock;
std::vector<Buffer*> g_buffers;
std::mutex g_str_lock;
std::unordered_map<std::string, uint32_t> g_str_ids;
uint32_t g_next_str = 1;
std::atomic<uint16_t> g_next_tid{1};
thread_local Buffer* t_buf = nullptr;

uint32_t parse_mask(const char* spec) {
    std::string s = spec;
    if (s == "1" || s.empty()) return SYSCALL | FS | RSX | TEX | TRAP;
    if (s == "all") return 0xFFFFFFFFu;
    uint32_t m = 0;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        std::string k = s.substr(pos, end - pos);
        if (k == "sys") m |= SYSCALL;
        else if (k == "import") m |= IMPORT;
        else if (k == "fs") m |= FS;
        else if (k == "rsx") m |= RSX;
        else if (k == "tex") m |= TEX;
        else if (k == "dma") m |= DMA;
        else if (k == "trap") m |= TRAP;
        else if (k == "draw") m |= DRAW;
        else if (k == "atomic") m |= ATOMIC;
        else if (!k.empty()) LOG_WARN("trace", "unknown category '%s'", k.c_str());
        pos = end + 1;
    }
    return m;
}

void write_out(Buffer* b) {
    if (!b->n) return;
    std::lock_guard lk(g_io);
    if (g_file) std::fwrite(b->recs, sizeof(Rec), b->n, g_file);
    b->n = 0;
}

std::string thread_name() {
    PWSTR w = nullptr;
    std::string name;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &w)) && w) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            name.resize(n - 1);
            WideCharToMultiByte(CP_UTF8, 0, w, -1, name.data(), n, nullptr, nullptr);
        }
        LocalFree(w);
    }
    if (name.empty()) name = "host " + std::to_string(GetCurrentThreadId());
    return name;
}

Buffer* buffer() {
    if (t_buf) return t_buf;
    auto* b = new Buffer();
    b->tid = g_next_tid.fetch_add(1);
    uint32_t name = intern(thread_name());
    b->recs[b->n++] = Rec{now_ns(), T_THREAD, b->tid, b->tid, name, GetCurrentThreadId(), 0, 0, 0};
    t_buf = b;
    std::lock_guard lk(g_reg_lock);
    g_buffers.push_back(b);
    return b;
}

}

uint64_t now_ns() { return logging::uptime_ns(); }

uint32_t intern(const std::string& s) {
    std::lock_guard lk(g_str_lock);
    auto it = g_str_ids.find(s);
    if (it != g_str_ids.end()) return it->second;
    uint32_t id = g_next_str++;
    g_str_ids.emplace(s, id);
    if (g_strings) {
        std::string esc;
        for (char ch : s) {
            if (ch == '\n') esc += "\\n";
            else if (ch == '\t') esc += "\\t";
            else if (ch == '\\') esc += "\\\\";
            else esc += ch;
        }
        std::fprintf(g_strings, "%u\t%s\n", id, esc.c_str());
    }
    return id;
}

void emit(uint16_t type, uint32_t x, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    Buffer* buf = buffer();
    while (buf->busy.test_and_set(std::memory_order_acquire)) {
    }
    buf->recs[buf->n++] = Rec{now_ns(), type, buf->tid, x, a, b, c, d, e};
    if (buf->n == kCap) write_out(buf);
    buf->busy.clear(std::memory_order_release);
}

void flush_all() {
    if (!g_file) return;
    std::vector<Buffer*> bufs;
    {
        std::lock_guard lk(g_reg_lock);
        bufs = g_buffers;
    }
    for (Buffer* b : bufs) {
        while (b->busy.test_and_set(std::memory_order_acquire)) {
        }
        write_out(b);
        b->busy.clear(std::memory_order_release);
    }
    std::lock_guard lk(g_io);
    std::fflush(g_file);
    std::lock_guard sk(g_str_lock);
    if (g_strings) std::fflush(g_strings);
}

void init() {
    const char* spec = std::getenv("GOWA_EVTRACE");
    if (!spec) return;
    std::string path = std::getenv("GOWA_EVTRACE_FILE") ? std::getenv("GOWA_EVTRACE_FILE") : "trace.bin";
    g_file = std::fopen(path.c_str(), "wb");
    g_strings = std::fopen((path + ".str").c_str(), "w");
    if (!g_file || !g_strings) {
        LOG_ERROR("trace", "cannot open %s", path.c_str());
        g_file = nullptr;
        return;
    }
    char hdr[16] = {'G', 'O', 'W', 'A', 'T', 'R', 'C', '1'};
    uint32_t ver = 1, size = sizeof(Rec);
    std::memcpy(hdr + 8, &ver, 4);
    std::memcpy(hdr + 12, &size, 4);
    std::fwrite(hdr, 1, 16, g_file);
    uint32_t mask = parse_mask(spec);
    double at = std::getenv("GOWA_EVTRACE_AT") ? std::atof(std::getenv("GOWA_EVTRACE_AT")) : 0.0;
    double end = std::getenv("GOWA_EVTRACE_END") ? std::atof(std::getenv("GOWA_EVTRACE_END")) : 0.0;
    LOG_INFO("trace", "event trace to %s mask 0x%x from %.1fs%s", path.c_str(), mask, at, end > 0 ? " (with end)" : "");
    if (at <= 0.0) g_mask = mask;
    std::thread([mask, at, end] {
        for (;;) {
            double now = logging::uptime();
            if (at > 0.0 && now >= at && !g_mask && (end <= 0.0 || now < end)) {
                g_mask = mask;
                LOG_INFO("trace", "event trace started");
            }
            if (end > 0.0 && now >= end && g_mask) {
                g_mask = 0;
                LOG_INFO("trace", "event trace stopped");
            }
            Sleep(250);
            flush_all();
        }
    }).detach();
}

void shutdown() {
    g_mask = 0;
    flush_all();
}

bool dump_memory(const char* path) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    for (uint64_t a = 0; a < 0x100000000ull;) {
        MEMORY_BASIC_INFORMATION mi{};
        if (!VirtualQuery(vm::g_base + a, &mi, sizeof mi)) break;
        uint64_t len = reinterpret_cast<uint8_t*>(mi.BaseAddress) + mi.RegionSize - (vm::g_base + a);
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            uint32_t hdr[2] = {static_cast<uint32_t>(a), static_cast<uint32_t>(len)};
            std::fwrite(hdr, 4, 2, f);
            std::fwrite(vm::g_base + a, 1, static_cast<size_t>(len), f);
        }
        a += len;
    }
    std::fclose(f);
    if (on(TRAP)) emit(T_MARK, 1, intern(path));
    LOG_INFO("trace", "memory dumped to %s", path);
    return true;
}

}
