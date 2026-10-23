#include "runtime/reservation.h"
#include "runtime/memory.h"

#include "runtime/log.h"

#include <algorithm>
#include <chrono>
#include <immintrin.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#pragma comment(lib, "Synchronization.lib")

namespace rsv {

namespace {

constexpr uint32_t PER_GROUP = 7;
constexpr uint32_t GROUPS = 1u << 14;
constexpr uint32_t GROUP_MASK = GROUPS - 1;

struct Entry {
    std::atomic<uint64_t> ver{0};
};

struct alignas(64) Group {
    std::atomic<uint64_t> lock{0};
    Entry e[PER_GROUP];
};
static_assert(sizeof(Group) == 64);

Group g_groups[GROUPS];

Entry& entry(uint32_t addr) {
    uint32_t l = addr >> 7;
    return g_groups[(l / PER_GROUP) & GROUP_MASK].e[l % PER_GROUP];
}

struct Cursor {
    uint32_t g, s;
    explicit Cursor(uint32_t addr) : g((addr >> 7) / PER_GROUP), s((addr >> 7) % PER_GROUP) {}
    Entry& operator*() const { return g_groups[g & GROUP_MASK].e[s]; }
    void next() {
        if (++s == PER_GROUP) {
            s = 0;
            g++;
        }
    }
};

Group& group_of(Entry& e) { return *reinterpret_cast<Group*>(reinterpret_cast<uintptr_t>(&e) & ~uintptr_t(63)); }

void lock_group(Group& g) {
    uint32_t spins = 0;
    for (;;) {
        uint64_t z = 0;
        if (g.lock.load(std::memory_order_relaxed) == 0 && g.lock.compare_exchange_weak(z, 1, std::memory_order_acquire, std::memory_order_relaxed)) return;
        if (++spins < 2000) {
            _mm_pause();
        } else {
            spins = 0;
            std::this_thread::yield();
        }
    }
}

void unlock_group(Group& g) { g.lock.store(0, std::memory_order_release); }

uint64_t mark_line(Entry& e) {
    uint64_t v = e.ver.load(std::memory_order_relaxed);
    e.ver.store(v + 64, std::memory_order_relaxed);
    std::atomic_signal_fence(std::memory_order_seq_cst);
    return v;
}

uint64_t lock_line(Entry& e) {
    lock_group(group_of(e));
    return mark_line(e);
}

void unlock_line(Entry& e, uint64_t v, bool wrote) {
    e.ver.store(wrote ? v + 128 : v, std::memory_order_release);
    unlock_group(group_of(e));
}

const bool g_logged = std::getenv("GOWA_RSV_LOG") != nullptr;

}

std::atomic<uint64_t>& version(uint32_t addr) { return entry(addr).ver; }

uint64_t snapshot(uint32_t line, uint8_t* out) {
    Entry& e = entry(line);
    for (;;) {
        uint64_t v1 = e.ver.load(std::memory_order_acquire);
        if (v1 & 64) {
            _mm_pause();
            continue;
        }
        std::memcpy(out, vm::ptr(line), 128);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (e.ver.load(std::memory_order_acquire) == v1) return v1;
    }
}

bool watched_line(uint32_t line) {
    static const std::vector<uint32_t> lines = [] {
        std::vector<uint32_t> v;
        if (const char* e = std::getenv("GOWA_RSV_LOG")) {
            std::string s = e;
            size_t pos = 0;
            while (pos < s.size()) {
                size_t end = s.find(',', pos);
                if (end == std::string::npos) end = s.size();
                v.push_back(static_cast<uint32_t>(std::strtoul(s.substr(pos, end - pos).c_str(), nullptr, 16)) & ~127u);
                pos = end + 1;
            }
        }
        return v;
    }();
    static const bool jm = std::getenv("GOWA_RSV_LOG") && std::string(std::getenv("GOWA_RSV_LOG")).rfind("jm", 0) == 0;
    if (jm && (vm::read32(line) == 0x007a7b00u || line == 0x3001ad00u)) return true;
    for (uint32_t l : lines) if (l == line) return true;
    return false;
}

void log_change(uint32_t line, uint32_t offset, const uint8_t* data, uint32_t size, const char* how) {
    if (!watched_line(line)) return;
    const uint8_t* cur = vm::ptr(line);
    char buf[512];
    int n = 0;
    for (uint32_t i = 0; i < size && n < 480; i++) {
        uint32_t o = offset + i;
        if (cur[o] != data[i]) n += std::snprintf(buf + n, sizeof buf - n, " [%02x]%02x->%02x", o, cur[o], data[i]);
    }
    if (n) LOG_INFO("rsv", "%s line 0x%08x:%s", how, line, buf);
}

bool commit_if(uint32_t line, uint64_t rtime, const uint8_t* expected, const uint8_t* data, uint32_t offset, uint32_t size, bool data_only) {
    Entry& e = entry(line);
    static const bool strict = std::getenv("GOWA_RSV_STRICT") != nullptr;
    if ((strict || !data_only) && e.ver.load(std::memory_order_acquire) != rtime) return false;
    uint64_t v0 = lock_line(e);
    if ((strict || !data_only) && v0 != rtime) {
        unlock_line(e, v0, false);
        return false;
    }
    if (std::memcmp(vm::ptr(line + offset), expected, size) != 0) {
        unlock_line(e, v0, false);
        return false;
    }
    if (g_logged) log_change(line, offset, data, size, size == 128 ? "putllc" : "stcx");
    uint8_t* dst = vm::ptr(line + offset);
    for (uint32_t i = 0; i < size; i += 8) {
        uint64_t want, was;
        std::memcpy(&want, data + i, 8);
        std::memcpy(&was, expected + i, 8);
        if (want == was) continue;
        for (uint32_t b = 0; b < 8; b++) {
            if (data[i + b] != expected[i + b]) dst[i + b] = data[i + b];
        }
    }
    unlock_line(e, v0, true);
    if (sleepers()) e.ver.notify_all();
    return true;
}

void store_unconditional(uint32_t line, const uint8_t* data) {
    Entry& e = entry(line);
    uint64_t v0 = lock_line(e);
    if (g_logged) log_change(line, 0, data, 128, "putlluc");
    std::memcpy(vm::ptr(line), data, 128);
    unlock_line(e, v0, true);
    if (sleepers()) e.ver.notify_all();
}

void store_locked(uint32_t addr, const void* data, uint32_t size) {
    Entry& e = entry(addr & ~127u);
    uint64_t v0 = lock_line(e);
    if (g_logged) log_change(addr & ~127u, addr & 127u, static_cast<const uint8_t*>(data), size, "store");
    std::memcpy(vm::ptr(addr), data, size);
    unlock_line(e, v0, true);
    if (sleepers()) e.ver.notify_all();
}

void store_range(uint32_t addr, const uint8_t* data, uint32_t size) {
    if (!size) return;
    const uint32_t first = addr >> 7;
    const uint32_t n = ((addr + size - 1) >> 7) - first + 1;
    if (n > 256) {
        uint32_t done = 0;
        while (done < size) {
            uint32_t chunk = (std::min)(size - done, 0x8000u - ((addr + done) & 0x7FFFu));
            store_range(addr + done, data + done, chunk);
            done += chunk;
        }
        return;
    }
    const uint32_t g0 = first / PER_GROUP;
    const uint32_t ng = (first + n - 1) / PER_GROUP - g0 + 1;
    const uint32_t gi0 = g0 & GROUP_MASK;
    const uint32_t split = gi0 + ng > GROUPS ? GROUPS - gi0 : ng;
    for (uint32_t k = 0; k < ng - split; k++) lock_group(g_groups[k]);
    for (uint32_t k = 0; k < split; k++) lock_group(g_groups[gi0 + k]);
    uint64_t saved[256];
    Cursor c(addr);
    for (uint32_t k = 0; k < n; k++, c.next()) saved[k] = mark_line(*c);
    if (g_logged) {
        uint32_t done = 0;
        while (done < size) {
            uint32_t a = addr + done;
            uint32_t chunk = (std::min)(size - done, 128 - (a & 127));
            log_change(a & ~127u, a & 127u, data + done, chunk, "store");
            done += chunk;
        }
    }
    std::memcpy(vm::ptr(addr), data, size);
    Cursor u(addr);
    for (uint32_t k = 0; k < n; k++, u.next()) (*u).ver.store(saved[k] + 128, std::memory_order_release);
    for (uint32_t k = 0; k < ng - split; k++) unlock_group(g_groups[k]);
    for (uint32_t k = 0; k < split; k++) unlock_group(g_groups[gi0 + k]);
    if (sleepers())
        for (uint32_t k = 0; k < n; k++) entry(addr + (k << 7)).ver.notify_all();
}

void merge_bits(uint32_t addr, const uint8_t* data, const uint8_t* base, uint32_t size) {
    Entry& e = entry(addr & ~127u);
    uint64_t v0 = lock_line(e);
    uint8_t* dst = vm::ptr(addr);
    for (uint32_t i = 0; i < size; i++) {
        uint8_t changed = data[i] ^ base[i];
        if (changed) dst[i] = static_cast<uint8_t>((dst[i] & ~changed) | (data[i] & changed));
    }
    unlock_line(e, v0, true);
    if (sleepers()) e.ver.notify_all();
}

bool sleepers() {
    static const bool on = std::getenv("GOWA_RSV_SLEEP_US") != nullptr;
    return on;
}

void wake(uint32_t addr) {
    if (sleepers()) entry(addr).ver.notify_all();
}

bool copy_if_changed(uint32_t addr, uint8_t* dst, uint32_t size) {
    Entry& e = entry(addr & ~127u);
    const uint8_t* src = vm::ptr(addr);
    bool changed = false;
    for (;;) {
        uint64_t v1 = e.ver.load(std::memory_order_acquire);
        if (v1 & 64) {
            _mm_pause();
            continue;
        }
        uint32_t i = 0;
        for (; i + 32 <= size; i += 32) {
            __m256i s = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
            __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
            if (static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(s, d))) != 0xFFFFFFFFu) {
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), s);
                changed = true;
            }
        }
        for (; i + 16 <= size; i += 16) {
            __m128i s = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
            __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + i));
            if (_mm_movemask_epi8(_mm_cmpeq_epi8(s, d)) != 0xFFFF) {
                _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), s);
                changed = true;
            }
        }
        for (; i < size; i++) {
            if (dst[i] != src[i]) {
                dst[i] = src[i];
                changed = true;
            }
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (e.ver.load(std::memory_order_acquire) == v1) return changed;
    }
}

bool copy_range_if_changed(uint32_t addr, uint8_t* dst, uint32_t size, uint32_t& dirty_lo, uint32_t& dirty_hi) {
    const uint8_t* src = vm::ptr(addr);
    bool any = false;
    uint32_t done = 0;
    Cursor c(addr);
    while (done < size) {
        uint32_t a = addr + done;
        uint32_t chunk = (std::min)(size - done, 128 - (a & 127));
        Entry& e = *c;
        for (;;) {
            uint64_t v1 = e.ver.load(std::memory_order_acquire);
            if (v1 & 64) {
                _mm_pause();
                continue;
            }
            bool changed = false;
            const uint8_t* s = src + done;
            uint8_t* d = dst + done;
            uint32_t i = 0;
            for (; i + 32 <= chunk; i += 32) {
                __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + i));
                __m256i y = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(d + i));
                if (static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(x, y))) != 0xFFFFFFFFu) {
                    _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + i), x);
                    changed = true;
                }
            }
            for (; i < chunk; i++) {
                if (d[i] != s[i]) {
                    d[i] = s[i];
                    changed = true;
                }
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (e.ver.load(std::memory_order_acquire) != v1) continue;
            if (changed) {
                any = true;
                dirty_lo = (std::min)(dirty_lo, done);
                dirty_hi = (std::max)(dirty_hi, done + chunk);
            }
            break;
        }
        done += chunk;
        c.next();
    }
    return any;
}

bool copy_range_prefetch(uint32_t addr, uint8_t* dst, uint32_t size, uint32_t& dirty_lo, uint32_t& dirty_hi) {
    for (uint32_t a = (addr & ~127u) + 128; a < addr + size && a < (addr & ~127u) + 128 * 9; a += 128) _mm_prefetch(reinterpret_cast<const char*>(&entry(a).ver), _MM_HINT_T0);
    const uint8_t* src = vm::ptr(addr);
    bool any = false;
    uint32_t done = 0;
    Cursor c(addr);
    while (done < size) {
        uint32_t a = addr + done;
        uint32_t chunk = (std::min)(size - done, 128 - (a & 127));
        if (done + chunk + 1024 < size) {
            _mm_prefetch(reinterpret_cast<const char*>(&entry(a + 1024).ver), _MM_HINT_T0);
            _mm_prefetch(reinterpret_cast<const char*>(src + done + 1024), _MM_HINT_T0);
            _mm_prefetch(reinterpret_cast<const char*>(src + done + 1024 + 64), _MM_HINT_T0);
        }
        Entry& e = *c;
        for (;;) {
            uint64_t v1 = e.ver.load(std::memory_order_acquire);
            if (v1 & 64) {
                _mm_pause();
                continue;
            }
            uint32_t diff = 0;
            const uint8_t* s = src + done;
            uint8_t* d = dst + done;
            uint32_t i = 0;
            for (; i + 32 <= chunk; i += 32) {
                __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + i));
                __m256i y = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(d + i));
                diff |= ~static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(x, y)));
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + i), x);
            }
            for (; i < chunk; i++) {
                diff |= d[i] != s[i];
                d[i] = s[i];
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (e.ver.load(std::memory_order_acquire) != v1) continue;
            if (diff) {
                any = true;
                dirty_lo = (std::min)(dirty_lo, done);
                dirty_hi = (std::max)(dirty_hi, done + chunk);
            }
            break;
        }
        done += chunk;
        c.next();
    }
    return any;
}

bool lost(uint32_t line, uint64_t rtime, const uint8_t* rdata) {
    static const bool strict = std::getenv("GOWA_RSV_STRICT") != nullptr;
    if (strict && entry(line).ver.load(std::memory_order_acquire) != rtime) return true;
    return std::memcmp(vm::ptr(line), rdata, 128) != 0;
}

void wait_change(uint32_t line, uint64_t rtime, uint32_t micros) {
    Entry& e = entry(line);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(micros);
    while (e.ver.load(std::memory_order_acquire) == rtime && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
}

namespace {
struct WaitStat {
    uint64_t calls = 0, ver = 0, data = 0, timeout = 0, us = 0;
    uint64_t hist[6] = {};
    std::map<uint32_t, uint64_t> offs;
};
std::mutex g_wait_lock;
std::map<uint32_t, WaitStat> g_wait_stats;
const bool g_wait_stats_on = std::getenv("GOWA_SPU_OCC") != nullptr;
uint64_t g_qpf = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<uint64_t>(f.QuadPart);
}();
uint64_t qpc() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart);
}
void note_wait(uint32_t line, uint64_t t0, uint64_t rtime, const uint8_t* rdata, int outcome) {
    uint64_t us = (qpc() - t0) * 1000000ull / g_qpf;
    bool ver_changed = entry(line).ver.load(std::memory_order_relaxed) != rtime;
    uint32_t off = 0xFFFF;
    if (outcome && !ver_changed) {
        const uint8_t* cur = vm::ptr(line);
        off = 0;
        for (uint32_t i = 0; i < 128; i++)
            if (cur[i] != rdata[i]) off = (off << 8) | (i + 1);
        off &= 0xFFFFFF;
    }
    std::lock_guard lk(g_wait_lock);
    WaitStat& w = g_wait_stats[line];
    w.calls++;
    w.us += us;
    int b = us < 10 ? 0 : us < 30 ? 1 : us < 100 ? 2 : us < 300 ? 3 : us < 900 ? 4 : 5;
    w.hist[b]++;
    if (outcome == 0) w.timeout++;
    else if (ver_changed) w.ver++;
    else {
        w.data++;
        w.offs[off]++;
    }
}
bool wait_lost_impl(uint32_t line, uint64_t rtime, const uint8_t* rdata, uint32_t micros);
}

std::string wait_stats_take() {
    std::vector<std::pair<uint32_t, WaitStat>> v;
    {
        std::lock_guard lk(g_wait_lock);
        v.assign(g_wait_stats.begin(), g_wait_stats.end());
        g_wait_stats.clear();
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.us > b.second.us; });
    std::string out;
    for (size_t i = 0; i < v.size() && i < 8; i++) {
        char b[160];
        std::snprintf(b, sizeof b, " [%08x n %llu %.1f ms ver %llu data %llu to %llu]", v[i].first, (unsigned long long)v[i].second.calls, v[i].second.us / 1000.0,
                      (unsigned long long)v[i].second.ver, (unsigned long long)v[i].second.data, (unsigned long long)v[i].second.timeout);
        out += b;
        std::snprintf(b, sizeof b, " hist<10/30/100/300/900/+ %llu %llu %llu %llu %llu %llu offs", (unsigned long long)v[i].second.hist[0], (unsigned long long)v[i].second.hist[1],
                      (unsigned long long)v[i].second.hist[2], (unsigned long long)v[i].second.hist[3], (unsigned long long)v[i].second.hist[4], (unsigned long long)v[i].second.hist[5]);
        out += b;
        std::vector<std::pair<uint32_t, uint64_t>> o(v[i].second.offs.begin(), v[i].second.offs.end());
        std::sort(o.begin(), o.end(), [](auto& a, auto& b) { return a.second > b.second; });
        for (size_t j = 0; j < o.size() && j < 6; j++) {
            std::snprintf(b, sizeof b, " %06x:%llu", o[j].first, (unsigned long long)o[j].second);
            out += b;
        }
    }
    return out;
}

bool wait_lost(uint32_t line, uint64_t rtime, const uint8_t* rdata, uint32_t micros) {
    if (!g_wait_stats_on) return wait_lost_impl(line, rtime, rdata, micros);
    uint64_t t0 = qpc();
    bool r = wait_lost_impl(line, rtime, rdata, micros);
    note_wait(line, t0, rtime, rdata, r ? 1 : 0);
    return r;
}

namespace {
bool wait_lost_impl(uint32_t line, uint64_t rtime, const uint8_t* rdata, uint32_t micros) {
    Entry& e = entry(line);
    for (int i = 0; i < 200; i++) {
        if (lost(line, rtime, rdata)) return true;
        _mm_pause();
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(micros);
    for (;;) {
        uint64_t v = e.ver.load(std::memory_order_acquire);
        if (lost(line, rtime, rdata)) return true;
        auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) return false;
        static const long sleep_after = std::getenv("GOWA_RSV_SLEEP_US") ? std::atol(std::getenv("GOWA_RSV_SLEEP_US")) : -1;
        static const long yield_after = std::getenv("GOWA_RSV_YIELD_US") ? std::atol(std::getenv("GOWA_RSV_YIELD_US")) : 30;
        if (sleep_after >= 0 && micros - left >= sleep_after) WaitOnAddress(&e.ver, &v, sizeof v, static_cast<DWORD>((left + 999) / 1000));
        else if (yield_after >= 0 && micros - left >= yield_after) SwitchToThread();
        else _mm_pause();
    }
}
}

namespace {
constexpr int PARK_SLOTS = 64;
std::atomic<uint64_t> g_park_used{0};
std::atomic<uint32_t> g_park_line[PARK_SLOTS];
struct alignas(64) ParkWord {
    std::atomic<uint32_t> w{0};
};
ParkWord g_park_word[PARK_SLOTS];
struct alignas(64) SpinRole {
    std::atomic<uint32_t> line{0};
};
SpinRole g_spin_role[16];
SpinRole& spin_role(uint32_t line) { return g_spin_role[(line >> 7) & 15]; }

long env_long(const char* name, long def) {
    const char* e = std::getenv(name);
    return e ? std::atol(e) : def;
}

const bool g_park_on = env_long("GOWA_SPU_PARK", 0) != 0;
const long g_park_spin_us = env_long("GOWA_SPU_PARK_SPIN_US", 30);
const long g_park_timeout_us = env_long("GOWA_SPU_PARK_TIMEOUT_US", 2000);

void kick_line(uint32_t line) {
    for (int i = 0; i < PARK_SLOTS; i++) {
        if (g_park_line[i].load(std::memory_order_relaxed) == line) {
            g_park_word[i].w.fetch_add(1, std::memory_order_seq_cst);
            WakeByAddressSingle(&g_park_word[i].w);
        }
    }
}

int poll_once(uint32_t line, uint64_t rtime, const uint8_t* rdata, int slot, uint32_t w0, const std::atomic<bool>& stop) {
    if (lost(line, rtime, rdata)) return 1;
    if (g_park_word[slot].w.load(std::memory_order_acquire) != w0) return 2;
    if (stop.load(std::memory_order_relaxed)) return 3;
    return 0;
}
}

bool park_enabled() { return g_park_on; }

int park_alloc() {
    uint64_t used = g_park_used.load();
    for (;;) {
        if (used == ~0ull) return -1;
        int i = static_cast<int>(_tzcnt_u64(~used));
        if (g_park_used.compare_exchange_weak(used, used | (1ull << i))) return i;
    }
}

void park_free(int slot) {
    if (slot >= 0) g_park_used.fetch_and(~(1ull << slot));
}

uint32_t park_word(int slot) { return g_park_word[slot].w.load(std::memory_order_acquire); }

void park_kick(int slot) {
    if (slot < 0 || !g_park_on) return;
    g_park_word[slot].w.fetch_add(1, std::memory_order_seq_cst);
    WakeByAddressSingle(&g_park_word[slot].w);
}

namespace {
int wait_parked_impl(uint32_t line, uint64_t rtime, const uint8_t* rdata, int slot, uint32_t w0, int64_t limit_us, const std::atomic<bool>& stop);
}

int wait_parked(uint32_t line, uint64_t rtime, const uint8_t* rdata, int slot, uint32_t w0, int64_t limit_us, const std::atomic<bool>& stop) {
    if (!g_wait_stats_on) return wait_parked_impl(line, rtime, rdata, slot, w0, limit_us, stop);
    uint64_t t0 = qpc();
    int r = wait_parked_impl(line, rtime, rdata, slot, w0, limit_us, stop);
    note_wait(line, t0, rtime, rdata, r);
    return r;
}

namespace {
int wait_parked_impl(uint32_t line, uint64_t rtime, const uint8_t* rdata, int slot, uint32_t w0, int64_t limit_us, const std::atomic<bool>& stop) {
    const uint64_t t0 = qpc();
    auto elapsed_us = [&] { return static_cast<int64_t>((qpc() - t0) * 1000000ull / g_qpf); };
    const int64_t limit = limit_us >= 0 ? limit_us : INT64_MAX;
    for (int i = 0; i < 200; i++) {
        if (int r = poll_once(line, rtime, rdata, slot, w0, stop)) return r;
        _mm_pause();
    }
    for (;;) {
        if (int r = poll_once(line, rtime, rdata, slot, w0, stop)) return r;
        int64_t t = elapsed_us();
        if (t >= limit) return 0;
        if (t >= g_park_spin_us) break;
        _mm_pause();
    }
    SpinRole& role = spin_role(line);
    for (;;) {
        uint32_t holder = 0;
        if (role.line.compare_exchange_strong(holder, line, std::memory_order_seq_cst)) {
            int r = 0;
            const uint64_t s0 = qpc();
            for (;;) {
                if ((r = poll_once(line, rtime, rdata, slot, w0, stop))) break;
                int64_t t = elapsed_us();
                if (t >= limit) break;
                if ((qpc() - s0) * 1000000ull / g_qpf >= 30) SwitchToThread();
                else _mm_pause();
            }
            role.line.exchange(0, std::memory_order_seq_cst);
            kick_line(line);
            return r;
        }
        if (holder != line) {
            for (;;) {
                if (int r = poll_once(line, rtime, rdata, slot, w0, stop)) return r;
                int64_t t = elapsed_us();
                if (t >= limit || t >= 1000) return 0;
                SwitchToThread();
            }
        }
        int64_t left = limit - elapsed_us();
        if (left < 1000) {
            for (;;) {
                if (int r = poll_once(line, rtime, rdata, slot, w0, stop)) return r;
                if (elapsed_us() >= limit) return 0;
                SwitchToThread();
            }
        }
        g_park_line[slot].exchange(line, std::memory_order_seq_cst);
        if (role.line.load(std::memory_order_seq_cst) != line) {
            g_park_line[slot].store(0, std::memory_order_relaxed);
            continue;
        }
        if (int r = poll_once(line, rtime, rdata, slot, w0, stop)) {
            g_park_line[slot].store(0, std::memory_order_relaxed);
            return r;
        }
        int64_t wait_us = (std::min<int64_t>)(left - 500, g_park_timeout_us);
        uint32_t expect = w0;
        WaitOnAddress(&g_park_word[slot].w, &expect, sizeof expect, static_cast<DWORD>((std::max<int64_t>)(wait_us / 1000, 1)));
        g_park_line[slot].store(0, std::memory_order_relaxed);
        if (int r = poll_once(line, rtime, rdata, slot, w0, stop)) return r;
        if (elapsed_us() >= limit) return 0;
    }
}
}

uint32_t group_wrap_addr(uint32_t from) {
    for (uint32_t l = from >> 7;; l++)
        if (((l / PER_GROUP) & GROUP_MASK) == GROUP_MASK && l % PER_GROUP == PER_GROUP - 1) return (l + 1) << 7;
}

}
