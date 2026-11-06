#include "edge_ovl.h"
#include "edge_ovl_gen.h"

#include "edge_adapters.h"
#include "runtime/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace edge::ovl {

namespace {

constexpr uint32_t kBuckets = 512;

struct Table {
    int16_t first[kBuckets];
    std::vector<int16_t> next;
    Table() {
        std::fill(std::begin(first), std::end(first), int16_t(-1));
        next.assign(kEntryCount, -1);
        for (size_t i = kEntryCount; i-- > 0;) {
            uint32_t b = bucket(kEntries[i].head);
            next[i] = first[b];
            first[b] = int16_t(i);
        }
    }
    static uint32_t bucket(uint64_t head) { return uint32_t((head * 0x9E3779B97F4A7C15ull) >> 55) & (kBuckets - 1); }
};

const Table& table() {
    static const Table t;
    return t;
}

struct Stats {
    std::atomic<uint64_t> c[kCounterCount];
    std::atomic<uint64_t> per_entry[256];
    std::atomic<uint64_t> per_native[32];
    std::mutex m;
    std::map<uint32_t, uint64_t> interp_pcs;
};

Stats& stats() {
    static Stats s;
    return s;
}

struct DeclineKey {
    int native;
    uint64_t key;
    bool operator<(const DeclineKey& o) const { return native != o.native ? native < o.native : key < o.key; }
};

struct DeclineInfo {
    uint64_t count = 0;
    uint32_t files = 0;
    uint32_t ptr = 0;
    std::string overlay;
};

struct Declines {
    std::mutex m;
    std::map<DeclineKey, DeclineInfo> keys;
    uint32_t files = 0;
    bool dirty = false;
};

Declines& declines() {
    static Declines d;
    return d;
}

constexpr const char* kDeclineDir = "edge_declines";
constexpr uint32_t kFilesPerKey = 8;
constexpr uint32_t kMaxFiles = 400;

uint64_t fnv(const uint8_t* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

uint32_t pw(const spu::Context& c, int r) { return c.gpr[r].u32[3]; }

void write_summary() {
    Declines& d = declines();
    std::vector<std::pair<DeclineKey, DeclineInfo>> rows;
    {
        std::lock_guard lk(d.m);
        if (!d.dirty) return;
        d.dirty = false;
        rows.assign(d.keys.begin(), d.keys.end());
    }
    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.second.count > b.second.count; });
    size_t na = 0;
    const Adapter* a = adapters(na);
    std::string tmp = std::string(kDeclineDir) + "/summary.txt.tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "# native key count overlay_ptr overlay files\n");
    for (auto& r : rows)
        std::fprintf(f, "%s %016llx %llu %05x %s %u\n", r.first.native >= 0 && size_t(r.first.native) < na ? a[r.first.native].name : "?", static_cast<unsigned long long>(r.first.key), static_cast<unsigned long long>(r.second.count),
                     r.second.ptr, r.second.overlay.empty() ? "-" : r.second.overlay.c_str(), r.second.files);
    std::fclose(f);
    std::error_code ec;
    std::filesystem::rename(tmp, std::string(kDeclineDir) + "/summary.txt", ec);
}

void logger() {
    uint64_t last[kCounterCount] = {};
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        Stats& s = stats();
        uint64_t cur[kCounterCount];
        for (int i = 0; i < kCounterCount; i++) cur[i] = s.c[i].load(std::memory_order_relaxed);
        std::string ov;
        for (size_t i = 0; i < kEntryCount && i < 256; i++) {
            uint64_t v = s.per_entry[i].exchange(0, std::memory_order_relaxed);
            if (!v) continue;
            char b[64];
            std::snprintf(b, sizeof b, " %s:%llu", kEntries[i].name, static_cast<unsigned long long>(v));
            ov += b;
        }
        std::string dec;
        size_t na = 0;
        const Adapter* a = adapters(na);
        for (size_t i = 0; i < na && i < 32; i++) {
            uint64_t v = s.per_native[i].exchange(0, std::memory_order_relaxed);
            if (!v) continue;
            char b[64];
            std::snprintf(b, sizeof b, " %s:%llu", a[i].name, static_cast<unsigned long long>(v));
            dec += b;
        }
        std::vector<std::pair<uint32_t, uint64_t>> pcs;
        {
            std::lock_guard lk(s.m);
            pcs.assign(s.interp_pcs.begin(), s.interp_pcs.end());
            s.interp_pcs.clear();
        }
        std::sort(pcs.begin(), pcs.end(), [](auto& x, auto& y) { return x.second > y.second; });
        std::string hot;
        for (size_t i = 0; i < pcs.size() && i < 8; i++) {
            char b[40];
            std::snprintf(b, sizeof b, " %05x:%llu", pcs[i].first, static_cast<unsigned long long>(pcs[i].second));
            hot += b;
        }
        auto d = [&](int k) { return static_cast<unsigned long long>(cur[k] - last[k]); };
        LOG_INFO("edgecov", "overlay runs %llu (%llu insns), native declines %llu, interpreted in edge jobs %llu blocks %llu insns; overlays:%s; declines:%s; interp pcs:%s", d(kRuns), d(kRunInsns), d(kDeclines), d(kInterpBlocks),
                 d(kInterpInsns), ov.c_str(), dec.c_str(), hot.c_str());
        std::memcpy(last, cur, sizeof last);
        if (capture_on()) write_summary();
    }
}

void start_logger() {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(logger).detach(); });
}

}

bool stats_on() {
    static const bool on = [] {
        const char* e = std::getenv("GOWA_EDGE_STATS");
        return !(e && e[0] == '0');
    }();
    return on;
}

const Entry* find(const uint8_t* ls, uint32_t pc) {
    if ((pc & 3) || pc + 8 > spu::LS_SIZE) return nullptr;
    uint64_t head;
    std::memcpy(&head, ls + pc, 8);
    const Table& t = table();
    for (int i = t.first[Table::bucket(head)]; i >= 0; i = t.next[i]) {
        const Entry& e = kEntries[i];
        if (e.head != head || pc + e.size > spu::LS_SIZE) continue;
        if (std::memcmp(ls + pc, e.code, e.size) == 0) return &e;
    }
    return nullptr;
}

int index_of(const Entry* e) { return int(e - kEntries); }
const Entry* entry(size_t i) { return i < kEntryCount ? &kEntries[i] : nullptr; }
size_t entry_count() { return kEntryCount; }
const char* entry_name(size_t i) { return i < kEntryCount ? kEntries[i].name : "?"; }

namespace {

struct Local {
    uint64_t runs = 0;
    uint64_t insns = 0;
    uint32_t per[256] = {};
    void flush() {
        count(kRuns, runs);
        count(kRunInsns, insns);
        for (size_t i = 0; i < kEntryCount && i < 256; i++)
            if (per[i]) {
                stats().per_entry[i].fetch_add(per[i], std::memory_order_relaxed);
                per[i] = 0;
            }
        runs = insns = 0;
    }
};

thread_local Local t_local;

}

bool run(spu::Context& c, spu::Host& h, const Entry* e) {
    uint64_t budget = c.executed ? c.executed : (1ull << 40);
    bool ok = e->run(c, h, c.pc, budget);
    if (ok && stats_on()) {
        Local& l = t_local;
        l.insns += c.executed;
        l.per[index_of(e) & 255]++;
        if (++l.runs >= 2048) l.flush();
    }
    return ok;
}

void count(Counter k, uint64_t v) {
    if (!stats_on()) return;
    start_logger();
    stats().c[k].fetch_add(v, std::memory_order_relaxed);
}

void count_entry(int idx) {
    if (idx >= 0 && idx < 256) stats().per_entry[idx].fetch_add(1, std::memory_order_relaxed);
}

void count_decline(int native) {
    if (!stats_on()) return;
    count(kDeclines, 1);
    if (native >= 0 && native < 32) stats().per_native[native].fetch_add(1, std::memory_order_relaxed);
}

void note_interp(uint32_t pc) {
    if (!stats_on()) return;
    count(kInterpBlocks, 1);
    Stats& s = stats();
    std::lock_guard lk(s.m);
    s.interp_pcs[pc]++;
}

bool capture_on() {
    static const bool on = [] {
        const char* e = std::getenv("GOWA_EDGE_CAPTURE_DECLINES");
        return e && *e && e[0] != '0';
    }();
    return on;
}

void capture_decline(int native, const uint8_t* ls, const spu::Context& c) {
    if (!capture_on()) return;
    start_logger();
    uint64_t key = 0;
    uint32_t ptr = 0;
    if (native == 1) {
        ptr = pw(c, 8) & 0x3fffc;
    } else if (native == 9) {
        uint32_t ctx = pw(c, 3) & 0x3fff0;
        if (ctx + 144 <= spu::LS_SIZE) ptr = ((uint32_t(ls[ctx + 140]) << 24) | (uint32_t(ls[ctx + 141]) << 16) | (uint32_t(ls[ctx + 142]) << 8) | ls[ctx + 143]) & 0x3fffc;
    } else if (native == 8) {
        uint32_t sp = pw(c, 1);
        uint32_t m = 0;
        if ((sp & 15) || sp < 0x2000 || sp > 0x3ffc0) m |= 1;
        if (pw(c, 8) != 0) m |= 2;
        if (pw(c, 11) > 0x2000) m |= 4;
        if ((pw(c, 3) & 0x3fff0) < 0x10000 || (pw(c, 3) & 0x3fff0) + 0x200 > 0x40000) m |= 8;
        const int rs[] = {4, 5, 6, 7, 9, 10, 12};
        for (int k = 0; k < 7; k++) {
            uint32_t a = pw(c, rs[k]);
            if (a == 0 && (rs[k] == 5 || rs[k] == 6 || rs[k] == 7)) continue;
            if ((a & 15) || a < 0x10c80 || a >= sp - 0x1000) m |= 16u << k;
        }
        key = m;
    }
    std::string name;
    if (ptr && ptr + 256 <= spu::LS_SIZE) {
        key = fnv(ls + ptr, 256);
        if (const Entry* e = find(ls, ptr)) name = e->name;
    }
    Declines& d = declines();
    uint32_t file_no = 0;
    bool save = false;
    {
        std::lock_guard lk(d.m);
        DeclineInfo& info = d.keys[DeclineKey{native, key}];
        if (!info.count) {
            info.ptr = ptr;
            info.overlay = name;
        }
        info.count++;
        d.dirty = true;
        if (info.files < kFilesPerKey && d.files < kMaxFiles) {
            file_no = info.files++;
            d.files++;
            save = true;
        }
    }
    if (!save) return;
    std::error_code ec;
    std::filesystem::create_directories(kDeclineDir, ec);
    char path[160];
    std::snprintf(path, sizeof path, "%s/n%d_%016llx_%u.bin", kDeclineDir, native, static_cast<unsigned long long>(key), file_no);
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    uint32_t hdr[6] = {0x4c434445u, 1, uint32_t(native), uint32_t(key), uint32_t(key >> 32), c.pc};
    std::fwrite(hdr, sizeof hdr, 1, f);
    std::fwrite(c.gpr, sizeof c.gpr, 1, f);
    std::fwrite(ls, 1, spu::LS_SIZE, f);
    std::fclose(f);
}

}
