#include "spu/aot.h"
#include "spu/context.h"
#include "hle.h"
#include "edge/edge_gpu.h"
#include "edge/edge_adapters.h"
#ifdef SPU_BENCH_LIVE
#include "runtime/memory.h"
#include "thread.h"
#include <set>
#endif

#include <intrin.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <thread>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <xmmintrin.h>

namespace {

struct Event {
    int32_t kind;
    uint32_t ch;
    uint32_t value;
    uint32_t pc;
    uint32_t srr0;
    uint8_t ie;
    uint8_t halted;
    uint8_t result;
    std::vector<std::pair<uint32_t, const uint8_t*>> lines;
};

struct Capture {
    std::vector<uint8_t> raw;
    uint32_t slot = 0;
    uint32_t insns = 0;
    uint32_t pc0 = 0, srr00 = 0, ie0 = 0;
    const uint8_t* gpr0 = nullptr;
    const uint8_t* ls0 = nullptr;
    std::vector<Event> events;
    uint32_t pc1 = 0, srr01 = 0, ie1 = 0;
    const uint8_t* gpr1 = nullptr;
    const uint8_t* ls1 = nullptr;
};

bool load(const std::string& path, Capture& c) {
    std::ifstream f(path, std::ios::binary);
    c.raw.assign(std::istreambuf_iterator<char>(f), {});
    const uint8_t* p = c.raw.data();
    const uint8_t* end = p + c.raw.size();
    auto u32 = [&]() {
        uint32_t v;
        std::memcpy(&v, p, 4);
        p += 4;
        return v;
    };
    if (c.raw.size() < 16 || u32() != 0x43555053u) return false;
    u32();
    c.slot = u32();
    c.insns = u32();
    c.pc0 = u32();
    c.srr00 = u32();
    c.ie0 = u32();
    c.gpr0 = p;
    p += 2048;
    c.ls0 = p;
    p += spu::LS_SIZE;
    uint32_t n = u32();
    c.events.resize(n);
    for (Event& e : c.events) {
        e.kind = static_cast<int32_t>(u32());
        e.ch = u32();
        e.value = u32();
        e.pc = u32();
        e.srr0 = u32();
        e.ie = p[0];
        e.halted = p[1];
        e.result = p[2];
        p += 4;
        uint32_t lines = u32();
        for (uint32_t i = 0; i < lines; i++) {
            uint32_t l = u32();
            e.lines.push_back({l, p});
            p += 128;
        }
    }
    c.pc1 = u32();
    c.srr01 = u32();
    c.ie1 = u32();
    c.gpr1 = p;
    p += 2048;
    c.ls1 = p;
    p += spu::LS_SIZE;
    return p <= end;
}

struct State {
    spu::Context c;
    uint8_t* ls;
    std::unique_ptr<std::atomic<uint32_t>[]> line_epoch{new std::atomic<uint32_t>[spu::LS_SIZE / 128]()};
    std::atomic<uint32_t> epoch{1};
    std::atomic<uint32_t> all_epoch{1};
    std::vector<uint32_t> valid;
    State() {
        ls = static_cast<uint8_t*>(_aligned_malloc(spu::LS_SIZE + 16, 128));
        size_t n = 0;
        spu::compiled_blocks(n);
        valid.assign(n ? n : 1, 0);
    }
    ~State() { _aligned_free(ls); }
    void reset(const Capture& cap) {
        c = spu::Context{};
        std::memcpy(c.gpr, cap.gpr0, 2048);
        c.pc = cap.pc0;
        c.srr0 = cap.srr00;
        c.interrupts_enabled = cap.ie0 != 0;
        std::memcpy(ls, cap.ls0, spu::LS_SIZE);
        std::memset(ls + spu::LS_SIZE, 0, 16);
        epoch.store(epoch.load() + 2);
        all_epoch.store(epoch.load());
        for (uint32_t i = 0; i < spu::LS_SIZE / 128; i++) line_epoch[i].store(0);
        std::fill(valid.begin(), valid.end(), 0u);
        c.ls = ls;
        c.line_epoch = line_epoch.get();
        c.epoch = &epoch;
        c.all_epoch = &all_epoch;
        c.block_valid = valid.data();
    }
    bool fresh(uint32_t pc, uint32_t words, uint32_t v) const {
        if (!v || all_epoch.load(std::memory_order_relaxed) > v) return false;
        for (uint32_t l = pc >> 7, last = (pc + words * 4 - 1) >> 7; l <= last && l < spu::LS_SIZE / 128; l++) {
            if (line_epoch[l].load(std::memory_order_relaxed) > v) return false;
        }
        return true;
    }
};

bool g_skip_same = true;
bool g_hle = false;
bool g_to_last_event = false;
std::string g_state_out;
uint64_t g_hle_calls = 0;
uint64_t g_hle_insns = 0;
bool g_hle_strict = false;
bool g_hle_profile = false;
uint64_t g_host_budget = 1ull << 16;
std::map<std::pair<uint32_t, uint32_t>, uint64_t>* g_exit_hist = std::getenv("GOWA_BENCH_EXITS") ? new std::map<std::pair<uint32_t, uint32_t>, uint64_t>() : nullptr;
struct HleStat {
    uint64_t cycles = 0;
    uint64_t insns = 0;
    uint64_t calls = 0;
};
std::map<std::string, HleStat> g_hle_stats;
spu::Context g_iso;
std::vector<uint8_t> g_iso_ls;
std::vector<uint64_t> g_counts;
size_t g_count_pos = 0;
bool g_have_counts = false;
uint64_t g_hle_tsc = 0;
std::FILE* g_dma_log = nullptr;
std::vector<uint32_t> g_watch;
std::FILE* g_watch_log = nullptr;
uint32_t g_watch_max = 1u << 30;

void watch_hit(const spu::Context& c) {
    static std::map<uint32_t, uint32_t> hits;
    if (++hits[c.pc] > g_watch_max) return;
    std::fprintf(g_watch_log, "pc %05x lr %05x sp %05x\n", c.pc, c.gpr[0].u32[3], c.gpr[1].u32[3]);
    for (int r = 3; r <= 12; r++) {
        const spu::v128& v = c.gpr[r];
        float f[4];
        std::memcpy(f, v.u32, 16);
        std::fprintf(g_watch_log, "  r%-3d %08x %08x %08x %08x | %g %g %g %g\n", r, v.u32[3], v.u32[2], v.u32[1], v.u32[0], f[3], f[2], f[1], f[0]);
    }
}
uint32_t g_mfc[6] = {};

enum { R_KERNEL, R_PM, R_JOB, R_COUNT };
enum { CH_READ, CH_WRITE, CH_COUNT_OP, CH_STOP };

struct JobKey {
    uint8_t img[16];
    uint32_t pm;
    uint32_t wid;
    bool operator<(const JobKey& o) const {
        int c = std::memcmp(img, o.img, 16);
        if (c) return c < 0;
        if (pm != o.pm) return pm < o.pm;
        return wid < o.wid;
    }
};

JobKey job_key(const uint8_t* ls) {
    JobKey k;
    std::memcpy(k.img, ls + 0x4000, 16);
    uint32_t v;
    std::memcpy(&v, ls + 0xA00, 4);
    k.pm = __builtin_bswap32(v);
    std::memcpy(&v, ls + 0x1dc, 4);
    k.wid = __builtin_bswap32(v);
    return k;
}

int region_of(uint32_t pc) { return pc < 0xA00 ? R_KERNEL : pc < 0x4000 ? R_PM : R_JOB; }

struct JobAcc {
    uint64_t block_cyc[R_COUNT] = {};
    uint64_t block_insns[R_COUNT] = {};
    uint64_t blocks[R_COUNT] = {};
    uint64_t host_cyc[R_COUNT] = {};
    uint64_t interp_insns[R_COUNT] = {};
    uint64_t interp_cyc[R_COUNT] = {};
    uint64_t spin_getllar[R_COUNT] = {};
    uint64_t chan[R_COUNT][4][32] = {};
    uint64_t mfc_cmd[R_COUNT][256] = {};
    uint64_t mfc_bytes[R_COUNT][256] = {};
    uint64_t hle_cyc = 0, hle_insns = 0, hle_calls = 0;
    uint64_t disp_cyc = 0, disp = 0;
    uint64_t entries = 0;
    void add(const JobAcc& a) {
        for (int r = 0; r < R_COUNT; r++) {
            block_cyc[r] += a.block_cyc[r];
            block_insns[r] += a.block_insns[r];
            blocks[r] += a.blocks[r];
            host_cyc[r] += a.host_cyc[r];
            interp_insns[r] += a.interp_insns[r];
            interp_cyc[r] += a.interp_cyc[r];
            spin_getllar[r] += a.spin_getllar[r];
            for (int k = 0; k < 4; k++)
                for (int c = 0; c < 32; c++) chan[r][k][c] += a.chan[r][k][c];
            for (int c = 0; c < 256; c++) {
                mfc_cmd[r][c] += a.mfc_cmd[r][c];
                mfc_bytes[r][c] += a.mfc_bytes[r][c];
            }
        }
        hle_cyc += a.hle_cyc;
        hle_insns += a.hle_insns;
        hle_calls += a.hle_calls;
        disp_cyc += a.disp_cyc;
        disp += a.disp;
        entries += a.entries;
    }
};

struct BlockStatJ {
    uint64_t cycles = 0;
    uint64_t insns = 0;
    uint64_t calls = 0;
};

using JobBlocks = std::map<std::pair<JobKey, uint32_t>, BlockStatJ>;

bool g_jobprof = false;
bool g_jobprof_rec = false;
std::map<JobKey, JobAcc> g_jobs;
std::map<JobKey, JobAcc> g_jobs_total;
JobBlocks g_job_blocks;
JobBlocks g_job_blocks_total;
JobAcc* g_cur_job = nullptr;
JobKey g_cur_key{};
int g_cur_region = R_JOB;
uint32_t g_last_getllar_ea[R_COUNT] = {};
uint32_t g_entry_pc = 0;

std::FILE* g_pmtrace = nullptr;
uint64_t g_pmtrace_lines = 0;
uint64_t g_pmtrace_max = 3000000;
bool g_pmtrace_injob = false;

void jobprof_merge(const std::map<JobKey, JobAcc>& jobs, const JobBlocks& blocks) {
    for (auto& [k, a] : jobs) g_jobs_total[k].add(a);
    for (auto& [k, b] : blocks) {
        BlockStatJ& t = g_job_blocks_total[k];
        t.cycles += b.cycles;
        t.insns += b.insns;
        t.calls += b.calls;
    }
}

void jobprof_update(const uint8_t* ls) {
    JobKey k = job_key(ls);
    if (g_cur_job && std::memcmp(&k, &g_cur_key, sizeof k) == 0) return;
    g_cur_key = k;
    g_cur_job = &g_jobs[k];
    uint32_t marker, entry;
    std::memcpy(&marker, ls + 0x4020, 4);
    std::memcpy(&entry, ls + 0x4010, 4);
    g_entry_pc = __builtin_bswap32(marker) == 0xc0dec0deu ? (0x4000 + __builtin_bswap32(entry)) & spu::PC_MASK : 0xFFFFFFFFu;
}

uint32_t list_bytes(const uint8_t* ls, uint32_t list, uint32_t size) {
    uint32_t total = 0, la = list & 0x3FFF8;
    for (uint32_t i = 0; i + 8 <= size && la + i + 8 <= spu::LS_SIZE; i += 8) total += ((uint32_t(ls[la + i + 2]) << 8) | ls[la + i + 3]) & 0x7FFF;
    return total;
}

void jobprof_op(const uint8_t* ls, int kind, uint32_t ch, uint32_t value) {
    if (!g_cur_job) return;
    JobAcc& a = *g_cur_job;
    int r = g_cur_region;
    a.chan[r][kind][ch & 31]++;
    if (kind != CH_WRITE || ch != 21) return;
    uint32_t cmd = value & 0xFF;
    a.mfc_cmd[r][cmd]++;
    bool list = (cmd & 0x04) && cmd < 0x50;
    a.mfc_bytes[r][cmd] += list ? list_bytes(ls, g_mfc[2], g_mfc[3]) : g_mfc[3];
    uint32_t line = g_mfc[2] & ~127u;
    if (cmd == 0xD0 && line == g_last_getllar_ea[r]) a.spin_getllar[r]++;
    g_last_getllar_ea[r] = cmd == 0xD0 ? line : 0;
}

enum { PH_DISP, PH_KERNEL, PH_PM, PH_JOB, PH_NATIVE, PH_INTERP, PH_HOST_K, PH_HOST_PM, PH_HOST_JOB, PH_IDLE };
bool g_jobsample = false;
bool g_js_on = false;
std::atomic<bool> g_js_active{false};
std::atomic<bool> g_js_run{false};
volatile uint32_t g_phase = PH_IDLE;
std::vector<JobKey> g_js_keys;
std::map<JobKey, uint32_t> g_js_index;
uint32_t g_js_cur = 0;
JobKey g_js_key{};
bool g_js_have = false;
std::map<uint32_t, uint64_t> g_js_hist;

uint32_t js_index(const uint8_t* ls) {
    JobKey k = job_key(ls);
    if (g_js_have && std::memcmp(&k, &g_js_key, sizeof k) == 0) return g_js_cur;
    g_js_have = true;
    g_js_key = k;
    auto it = g_js_index.find(k);
    if (it == g_js_index.end()) {
        it = g_js_index.emplace(k, static_cast<uint32_t>(g_js_keys.size())).first;
        g_js_keys.push_back(k);
    }
    g_js_cur = it->second;
    return g_js_cur;
}

void js_sampler(uint64_t interval) {
    std::map<uint32_t, uint64_t> hist;
    while (g_js_run.load(std::memory_order_relaxed)) {
        uint64_t until = __rdtsc() + interval;
        while (__rdtsc() < until) _mm_pause();
        if (g_js_active.load(std::memory_order_relaxed)) {
            uint32_t ph = g_phase;
            hist[ph]++;
        }
    }
    g_js_hist = std::move(hist);
}

struct PhaseGuard {
    uint32_t saved;
    PhaseGuard() : saved(g_phase) {
        if (g_js_on) {
            uint32_t ph = saved & 15;
            uint32_t h = ph == PH_KERNEL ? PH_HOST_K : ph == PH_PM ? PH_HOST_PM : PH_HOST_JOB;
            g_phase = (saved & ~15u) | h;
        }
    }
    ~PhaseGuard() {
        if (g_js_on) g_phase = saved;
    }
};

class Replay : public spu::Host {
public:
    State& s;
    const Capture& cap;
    size_t pos = 0;
    std::string error;
    Replay(State& st, const Capture& c) : s(st), cap(c) {}
    void apply_lines(const Event& e) {
        uint32_t ep = 0;
        for (auto& [l, d] : e.lines) {
            if (g_skip_same && std::memcmp(s.ls + l, d, 128) == 0) continue;
            if (!ep) ep = s.epoch.fetch_add(1) + 1;
            std::memcpy(s.ls + l, d, 128);
            s.line_epoch[l >> 7].store(ep);
        }
    }
    const Event* next(int kind, uint32_t ch, uint32_t value, bool check) {
        if (pos >= cap.events.size()) {
            if (error.empty()) error = "extra channel op kind " + std::to_string(kind) + " ch " + std::to_string(ch);
            s.c.halted = true;
            return nullptr;
        }
        const Event& e = cap.events[pos++];
        if (e.kind != kind || e.ch != ch || (check && e.value != value)) {
            if (error.empty()) {
                char b[200];
                std::snprintf(b, sizeof b, "event %zu: got kind %d ch %u val 0x%x, recorded kind %d ch %u val 0x%x", pos - 1, kind, ch, value, e.kind, e.ch, e.value);
                error = b;
            }
            s.c.halted = true;
        }
        if (!e.lines.empty()) apply_lines(e);
        s.c.pc = e.pc;
        s.c.srr0 = e.srr0;
        s.c.interrupts_enabled = e.ie != 0;
        if (e.halted) s.c.halted = true;
        return &e;
    }
    struct HostTimer {
        uint64_t t0;
        HostTimer() : t0(g_jobprof_rec ? __rdtsc() : 0) {}
        ~HostTimer() {
            if (g_jobprof_rec && g_cur_job) g_cur_job->host_cyc[g_cur_region] += __rdtsc() - t0;
        }
    };
    uint32_t read_channel(uint32_t ch) override {
        HostTimer ht;
        PhaseGuard pg;
        if (g_jobprof_rec) jobprof_op(s.ls, CH_READ, ch, 0);
        if (g_pmtrace && g_pmtrace_lines < g_pmtrace_max && pos < cap.events.size()) {
            g_pmtrace_lines++;
            std::fprintf(g_pmtrace, "  rdch %u -> %08x\n", ch, cap.events[pos].value);
        }
        const Event* e = next(0, ch, 0, false);
        return e ? e->value : 0;
    }
    void write_channel(uint32_t ch, uint32_t value) override {
        HostTimer ht;
        PhaseGuard pg;
        if (g_jobprof_rec || g_pmtrace) {
            if (ch >= 16 && ch <= 20) g_mfc[ch - 16] = value;
            if (g_jobprof_rec) jobprof_op(s.ls, CH_WRITE, ch, value);
        }
        if (g_pmtrace && g_pmtrace_lines < g_pmtrace_max && (ch < 16 || ch > 20)) {
            g_pmtrace_lines++;
            if (ch == 21) {
                uint32_t cmd = value & 0xFF;
                std::fprintf(g_pmtrace, "  mfc %02x lsa %05x ea %08x size %x tag %u", cmd, g_mfc[0] & 0x3FFFF, g_mfc[2], g_mfc[3], g_mfc[4]);
                if (cmd == 0xB4 || cmd == 0xB0 || cmd == 0xB8 || ((cmd & 0xF0) == 0x20 && g_mfc[3] <= 0x80 && (g_mfc[0] & 0x3FFFF) + g_mfc[3] <= spu::LS_SIZE)) {
                    uint32_t n = cmd >= 0xB0 ? 128 : g_mfc[3], l = g_mfc[0] & 0x3FF80;
                    if (cmd < 0xB0) l = g_mfc[0] & 0x3FFFF;
                    std::fprintf(g_pmtrace, " data ");
                    for (uint32_t i = 0; i < n; i++) std::fprintf(g_pmtrace, "%02x", s.ls[l + i]);
                }
                if ((cmd & 0x04) && cmd < 0x50) {
                    uint32_t la = g_mfc[2] & 0x3FFF8;
                    std::fprintf(g_pmtrace, " list");
                    for (uint32_t i = 0; i + 8 <= g_mfc[3] && la + i + 8 <= spu::LS_SIZE; i += 8) {
                        uint32_t sz = ((uint32_t(s.ls[la + i + 2]) << 8) | s.ls[la + i + 3]) & 0x7FFF, ea;
                        std::memcpy(&ea, s.ls + la + i + 4, 4);
                        std::fprintf(g_pmtrace, " %08x:%x", __builtin_bswap32(ea), sz);
                    }
                }
                std::fprintf(g_pmtrace, "\n");
            } else {
                std::fprintf(g_pmtrace, "  wrch %u %08x\n", ch, value);
            }
        }
        if (g_dma_log && ch >= 16 && ch <= 21) {
            g_mfc[ch - 16] = value;
            if (ch == 21) {
                uint32_t cmd = value & 0xFF, lsa = g_mfc[0] & 0x3FFFF, ea = g_mfc[2], size = g_mfc[3];
                bool put = (cmd & 0xF0) == 0x20 || (cmd & 0xF0) == 0x30;
                std::fprintf(g_dma_log, "%s cmd %02x pc %05x lsa %05x ea %08x size %x", put ? "PUT" : "GET", cmd, s.c.pc, lsa, ea, size);
                if ((cmd & 0x04) && cmd < 0x50 && std::getenv("SPU_BENCH_DMA_LIST")) {
                    uint32_t la = ea & 0x3FFF8;
                    for (uint32_t i = 0; i + 8 <= size && la + i + 8 <= spu::LS_SIZE; i += 8) {
                        const uint8_t* e = s.ls + la + i;
                        std::fprintf(g_dma_log, " elem %08x %x", (uint32_t(e[4]) << 24) | (uint32_t(e[5]) << 16) | (uint32_t(e[6]) << 8) | e[7], ((uint32_t(e[2]) << 8) | e[3]) & 0x7FFF);
                    }
                } else if (put && size && lsa + size <= spu::LS_SIZE && size <= 0x4000 && !std::getenv("SPU_BENCH_DMA_LIST")) {
                    std::fprintf(g_dma_log, " data");
                    for (uint32_t i = 0; i < size; i++) std::fprintf(g_dma_log, "%s%02x", (i & 63) ? "" : " ", s.ls[lsa + i]);
                }
                std::fprintf(g_dma_log, "\n");
            }
        }
        next(1, ch, value, true);
        if (g_pmtrace && g_pmtrace_lines < g_pmtrace_max && ch == 21) {
            uint32_t cmd = value & 0xFF, lsa = g_mfc[0] & 0x3FFFF, size = g_mfc[3];
            if (cmd == 0xD0) {
                lsa &= 0x3FF80;
                size = 128;
            }
            if ((cmd == 0xD0 || (cmd & 0xF4) == 0x40) && size <= 0x100 && lsa + size <= spu::LS_SIZE) {
                g_pmtrace_lines++;
                std::fprintf(g_pmtrace, "    data ");
                for (uint32_t i = 0; i < size; i++) std::fprintf(g_pmtrace, "%02x%s", s.ls[lsa + i], (i & 15) == 15 ? " " : "");
                std::fprintf(g_pmtrace, "\n");
            }
        }
        if (g_dma_log && ch == 21) {
            uint32_t cmd = value & 0xFF, lsa = g_mfc[0] & 0x3FFFF, size = g_mfc[3];
            bool get = (cmd & 0xF0) == 0x40;
            if (get && size && size <= 0x4000 && lsa + size <= spu::LS_SIZE && !std::getenv("SPU_BENCH_DMA_LIST")) {
                std::fprintf(g_dma_log, "GETDATA lsa %05x size %x data", lsa, size);
                for (uint32_t i = 0; i < size; i++) std::fprintf(g_dma_log, "%s%02x", (i & 63) ? "" : " ", s.ls[lsa + i]);
                std::fprintf(g_dma_log, "\n");
            }
        }
    }
    uint32_t channel_count(uint32_t ch) override {
        HostTimer ht;
        PhaseGuard pg;
        if (g_jobprof_rec) jobprof_op(s.ls, CH_COUNT_OP, ch, 0);
        if (g_pmtrace && g_pmtrace_lines < g_pmtrace_max && pos < cap.events.size()) {
            g_pmtrace_lines++;
            std::fprintf(g_pmtrace, "  rchcnt %u -> %u\n", ch, cap.events[pos].value);
        }
        const Event* e = next(2, ch, 0, false);
        return e ? e->value : 0;
    }
    bool stop_and_signal(uint32_t code) override {
        HostTimer ht;
        PhaseGuard pg;
        if (g_jobprof_rec) jobprof_op(s.ls, CH_STOP, code & 31, 0);
        const Event* e = next(3, code, 0, false);
        return e ? e->result != 0 : false;
    }
    void halt(uint32_t pc, uint32_t word) override { next(4, pc, word, true); }
    bool interrupt_pending() override { return pos < cap.events.size() && cap.events[pos].kind == 5; }
};


#ifdef SPU_BENCH_LIVE
std::set<uint32_t> g_live_blocks;
spu::SpuThread* g_live_thread = nullptr;
uint64_t g_live_mismatch = 0;
bool g_live_check = true;
std::string g_live_first;

class Live : public Replay {
public:
    spu::SpuThread& t;
    uint32_t mfc[6] = {};
    Live(State& st, const Capture& c, spu::SpuThread& th) : Replay(st, c), t(th) {}
    void note(const char* what, uint32_t a, uint32_t b) {
        g_live_mismatch++;
        if (g_live_first.empty()) {
            char buf[200];
            std::snprintf(buf, sizeof buf, "event %zu pc %05x: %s 0x%x vs 0x%x", pos, s.c.pc, what, a, b);
            g_live_first = buf;
        }
    }
    static void ensure(uint32_t ea, uint32_t size) {
        if (!size) return;
        for (uint64_t a = ea & ~0xFFFFull; a < uint64_t(ea) + size; a += 0x10000) {
            if (g_live_blocks.insert(uint32_t(a)).second) vm::map(uint32_t(a), 0x10000);
        }
    }
    void expected(uint32_t lsa, uint32_t size, uint8_t* out) const {
        for (uint32_t i = 0; i < size;) {
            uint32_t a = (lsa + i) & 0x3FFFF;
            uint32_t n = (std::min)(size - i, spu::LS_SIZE - a);
            std::memcpy(out + i, s.ls + a, n);
            i += n;
        }
        if (pos >= cap.events.size()) return;
        for (auto& [line, d] : cap.events[pos].lines) {
            if (((line - lsa) & 0x3FFFF) >= size && ((lsa - line) & 0x3FFFF) >= 128) continue;
            for (uint32_t k = 0; k < 128; k++) {
                uint32_t off = (line + k - lsa) & 0x3FFFF;
                if (off < size) out[off] = d[k];
            }
        }
    }
    void seed(uint32_t ea, uint32_t lsa, uint32_t size) {
        if (!size || ea >= 0xF0000000u || size > 0x4000) return;
        ensure(ea, size);
        expected(lsa, size, vm::ptr(ea));
    }
    void check(uint32_t lsa, uint32_t size) {
        if (!g_live_check || !size || size > 0x4000) return;
        static uint8_t want[0x4000];
        expected(lsa, size, want);
        for (uint32_t i = 0; i < size; i++) {
            if (s.ls[(lsa + i) & 0x3FFFF] != want[i]) {
                note("ls byte", (lsa + i) & 0x3FFFF, want[i]);
                return;
            }
        }
    }
    template <class F>
    void walk_list(uint32_t lsa, uint32_t list, uint32_t size, F&& f) {
        uint32_t cur = lsa & 0x3FFF0;
        list &= 0x3FFF8;
        while (size >= 8) {
            const uint8_t* e = s.ls + list;
            bool stall = (e[0] & 0x80) != 0;
            uint32_t tsize = ((uint32_t(e[2]) << 8) | e[3]) & 0x7FFF;
            uint32_t ea;
            std::memcpy(&ea, e + 4, 4);
            ea = __builtin_bswap32(ea);
            f(ea, cur | (ea & 0xF), tsize);
            cur = (cur + ((tsize + 15) & ~15u)) & 0x3FFF0;
            list = (list + 8) & 0x3FFF8;
            size -= 8;
            if (stall && size >= 8) return;
        }
    }
    uint32_t next_status(uint32_t ch) const {
        for (size_t i = pos + 1; i < cap.events.size() && i < pos + 64; i++)
            if (cap.events[i].kind == 0 && cap.events[i].ch == ch) return cap.events[i].value;
        return 0xFFFFFFFFu;
    }
    void write_channel(uint32_t ch, uint32_t value) override {
        if (ch >= 16 && ch <= 20) {
            mfc[ch - 16] = value;
            t.write_channel(ch, value);
        } else if (ch == 22 || ch == 23) {
            t.write_channel(ch, value);
        } else if (ch == 26) {
            uint32_t tag = value & 31;
            auto st = t.stalled[tag];
            if (st.active) walk_list(st.lsa, st.list, st.remaining, [&](uint32_t ea, uint32_t l, uint32_t n) { if (((st.cmd & 0xF0) == 0x40)) seed(ea, l, n); });
            t.write_channel(ch, value);
            if (st.active) walk_list(st.lsa, st.list, st.remaining, [&](uint32_t ea, uint32_t l, uint32_t n) { if (((st.cmd & 0xF0) == 0x40) && ea < 0xF0000000u) check(l, n); });
        } else if (ch == 21) {
            uint32_t cmd = value & 0xFF, lsa = mfc[0] & 0x3FFFF, ea = mfc[2], size = mfc[3];
            bool list = (cmd & 0x04) && cmd < 0x50;
            bool get = (cmd & 0xF0) == 0x40;
            bool forward = true;
            if (cmd == 0xD0) {
                ensure(ea & ~127u, 128);
                seed(ea & ~127u, lsa & 0x3FF80, 128);
            } else if (cmd == 0xB4) {
                ensure(ea & ~127u, 128);
                if (next_status(27) == 1) vm::ptr(ea & ~127u)[0] ^= 0xA5;
            } else if (cmd == 0xB0 || cmd == 0xB8) {
                ensure(ea & ~127u, 128);
            } else if (list) {
                walk_list(lsa, ea, size, [&](uint32_t e, uint32_t l, uint32_t n) {
                    if (e >= 0xF0000000u) return;
                    ensure(e, n);
                    if (get) seed(e, l, n);
                });
            } else if ((cmd & 0xF0) == 0x20 || (cmd & 0xF0) == 0x30 || (cmd & 0xF0) == 0x40 || (cmd & 0xF0) == 0xA0 || cmd == 0x89) {
                if (ea >= 0xF0000000u) forward = false;
                else {
                    ensure(ea, size ? size : 4);
                    if (get) seed(ea, lsa, size);
                }
            }
            if (forward) t.write_channel(ch, value);
            if (cmd == 0xD0) check(lsa & 0x3FF80, 128);
            else if (list && get) walk_list(lsa, ea, size, [&](uint32_t e, uint32_t l, uint32_t n) { if (e < 0xF0000000u) check(l, n); });
            else if (get && forward && !list) check(lsa, size);
        }
        Replay::write_channel(ch, value);
    }
    bool live_count(uint32_t ch) const {
        return ch == 24 ? t.tag_stat_count : ch == 25 ? t.stall_stat_count : t.atomic_stat_count;
    }
    uint32_t read_channel(uint32_t ch) override {
        if ((ch == 24 || ch == 25 || ch == 27) && pos < cap.events.size()) {
            uint32_t want = cap.events[pos].value;
            if (!live_count(ch)) note("no live status for channel", ch, want);
            else {
                uint32_t got = t.read_channel(ch);
                if (got != want) note(ch == 27 ? "atomic status" : ch == 24 ? "tag status" : "stall status", got, want);
            }
        }
        return Replay::read_channel(ch);
    }
    uint32_t channel_count(uint32_t ch) override {
        if ((ch == 24 || ch == 25 || ch == 27) && pos < cap.events.size()) {
            uint32_t got = live_count(ch) ? 1 : 0;
            if (got != cap.events[pos].value) note("status count", ch, cap.events[pos].value);
        }
        return Replay::channel_count(ch);
    }
};
#endif

std::unique_ptr<Replay> make_host(State& s, const Capture& cap) {
#ifdef SPU_BENCH_LIVE
    if (g_live_thread) return std::make_unique<Live>(s, cap, *g_live_thread);
#endif
    return std::make_unique<Replay>(s, cap);
}

struct Slot {
    const spu::CompiledBlock* b = nullptr;
    uint32_t valid = 0;
};

struct BlockStat {
    uint64_t cycles = 0;
    uint64_t insns = 0;
    uint64_t calls = 0;
};

std::map<const spu::CompiledBlock*, BlockStat> g_stats;
bool g_profile = false;
bool g_sample = false;
std::atomic<bool> g_sampling{false};
std::map<uint64_t, uint64_t> g_rips;

void sampler(HANDLE target) {
    while (g_sampling.load()) {
        uint64_t until = __rdtsc() + 400000;
        while (__rdtsc() < until) _mm_pause();
        if (SuspendThread(target) == static_cast<DWORD>(-1)) return;
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL;
        bool ok = GetThreadContext(target, &c) != 0;
        ResumeThread(target);
        if (ok) g_rips[c.Rip]++;
    }
}

void report_samples() {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    std::map<std::string, uint64_t> by_fn;
    uint64_t total = 0;
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    for (auto& [rip, n] : g_rips) {
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        std::string name = SymFromAddr(GetCurrentProcess(), rip, &disp, sym) ? std::string(sym->Name) : "?";
        by_fn[name] += n;
        total += n;
    }
    std::vector<std::pair<std::string, uint64_t>> v(by_fn.begin(), by_fn.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < v.size() && i < 30; i++) std::printf("  %5.1f%% %s\n", 100.0 * v[i].second / total, v[i].first.c_str());
    std::FILE* f = std::fopen("spu_bench_rips.txt", "w");
    if (f) {
        for (auto& [rip, n] : g_rips) {
            auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            DWORD64 disp = 0;
            if (SymFromAddr(GetCurrentProcess(), rip, &disp, sym)) std::fprintf(f, "%llu %s+0x%llx\n", static_cast<unsigned long long>(n), sym->Name, static_cast<unsigned long long>(disp));
        }
        std::fclose(f);
    }
}

struct Result {
    uint64_t cycles = 0;
    uint64_t insns = 0;
    uint64_t dispatches = 0;
    uint64_t interpreted = 0;
    std::string error;
    bool exact = false;
};

class IsoHost : public spu::Host {
public:
    bool failed = false;
    uint32_t read_channel(uint32_t) override {
        failed = true;
        return 0;
    }
    void write_channel(uint32_t, uint32_t) override { failed = true; }
    uint32_t channel_count(uint32_t) override {
        failed = true;
        return 0;
    }
    bool stop_and_signal(uint32_t) override {
        failed = true;
        return false;
    }
    void halt(uint32_t, uint32_t) override { failed = true; }
};

bool iso_count(const State& s, uint64_t& k, uint32_t ret, uint32_t sp) {
    g_iso_ls.assign(s.ls, s.ls + spu::LS_SIZE + 16);
    g_iso = s.c;
    g_iso.ls = g_iso_ls.data();
    g_iso.line_epoch = nullptr;
    g_iso.halted = false;
    IsoHost host;
    for (k = 0; k < 50000000;) {
        spu::step(g_iso, host);
        k++;
        if (host.failed) return false;
        if (g_iso.pc == ret && g_iso.gpr[1].u32[3] == sp) return true;
    }
    return false;
}

Result run(State& s, const Capture& cap, bool regions) {
    Result r;
    s.reset(cap);
    std::unique_ptr<Replay> host_ptr = make_host(s, cap);
    Replay& host = *host_ptr;
    std::vector<Slot> cache(spu::LS_SIZE / 4);
    struct HleSlot {
        const spu::hle::Function* f;
        uint32_t valid;
    };
    static const bool hle_cache_on = [] {
        const char* e = std::getenv("GOWA_SPU_HLE_CACHE");
        return !e || e[0] != '0';
    }();
    std::vector<HleSlot> hcache(hle_cache_on ? spu::LS_SIZE / 4 : 0, HleSlot{});
    auto hle_find = [&](uint32_t pc) -> const spu::hle::Function* {
        if (!hle_cache_on) return spu::hle::lookup(s.ls, pc);
        HleSlot& hs = hcache[pc >> 2];
        uint32_t w = spu::hle::max_sig_words(pc);
        if (hs.valid && s.fresh(pc, w, hs.valid)) return hs.f;
        uint32_t e = s.epoch.load();
        const spu::hle::Function* f = spu::hle::lookup(s.ls, pc);
        hs.f = f;
        hs.valid = w < 255 ? e : 0;
        return f;
    };
    uint64_t n = 0;
    ULONG64 t0 = 0;
    QueryThreadCycleTime(GetCurrentThread(), &t0);
    bool to_last = g_to_last_event;
    uint64_t excluded = 0;
    g_jobprof_rec = g_jobprof && (!g_hle || g_have_counts);
    g_cur_job = nullptr;
    uint64_t jp_top = 0;
    g_js_on = g_jobsample && (!g_hle || g_have_counts);
    uint32_t js_base = 0;
    if (g_js_on) {
        g_js_have = false;
        g_js_active.store(true);
    }
    while (!s.c.halted && (to_last ? host.pos < cap.events.size() : n < cap.insns)) {
        if (g_pmtrace && g_pmtrace_lines < g_pmtrace_max && (!g_hle || g_have_counts)) {
            bool injob = s.c.pc >= 0x4000 && s.c.pc < 0x3FE00;
            if (!injob) {
                g_pmtrace_lines++;
                uint32_t w;
                std::memcpy(&w, s.ls + 0x1dc, 4);
                std::fprintf(g_pmtrace, "B %05x r0 %05x r3 %08x r4 %08x r5 %08x\n", s.c.pc, s.c.gpr[0].u32[3], s.c.gpr[3].u32[3], s.c.gpr[4].u32[3], s.c.gpr[5].u32[3]);
            } else if (!g_pmtrace_injob) {
                g_pmtrace_lines++;
                std::fprintf(g_pmtrace, "J %05x r0 %05x r3 %08x r4 %08x r5 %08x n %llu\n", s.c.pc, s.c.gpr[0].u32[3], s.c.gpr[3].u32[3], s.c.gpr[4].u32[3], s.c.gpr[5].u32[3], (unsigned long long)n);
            }
            g_pmtrace_injob = injob;
        }
        if (g_js_on) {
            js_base = js_index(s.ls) << 4;
            g_phase = js_base | PH_DISP;
        }
        if (g_jobprof_rec) {
            jobprof_update(s.ls);
            g_cur_region = region_of(s.c.pc);
            if (s.c.pc == g_entry_pc) g_cur_job->entries++;
            g_cur_job->disp++;
            jp_top = __rdtsc();
        }
        if (g_hle && (spu::hle::any_at(s.c.pc) || spu::hle::dyn_at(s.c.pc))) {
            const spu::hle::Function* f = spu::hle::any_at(s.c.pc) ? hle_find(s.c.pc) : nullptr;
            if (!f && spu::hle::dyn_at(s.c.pc)) f = spu::hle::lookup_dyn(s.ls, s.c.pc);
            auto try_host = [&](const spu::hle::Function* f) -> bool {
                if (cap.insns - n > g_host_budget + 200000) {
                    uint32_t pc0 = s.c.pc;
                    uint64_t hb = g_host_budget;
                    if (host.pos < cap.events.size() && cap.events[host.pos].kind == 5 && cap.events[host.pos].value > n && cap.events[host.pos].value - n < hb) hb = cap.events[host.pos].value - n;
                    s.c.executed = uint32_t(hb);
                    if (g_js_on) g_phase = js_base | PH_NATIVE;
                    uint64_t h0 = __rdtsc();
                    bool handled = f->host_handler(s.c, host);
                    uint64_t hdt = __rdtsc() - h0;
                    if (g_js_on) g_phase = js_base | PH_DISP;
                    if (handled) {
                        uint64_t k = s.c.executed;
                        if (g_exit_hist) (*g_exit_hist)[{pc0, s.c.pc}] += 1;
                        uint64_t target = n + k;
                        if (host.pos < cap.events.size() && cap.events[host.pos].kind == 5 && cap.events[host.pos].value < target && host.error.empty()) {
                            char b[160];
                            std::snprintf(b, sizeof b, "%s at 0x%05x ran past irq event at insn %u (native ended at %llu)", f->name, pc0, cap.events[host.pos].value, (unsigned long long)target);
                            host.error = b;
                            s.c.halted = true;
                        }
                        n = target;
                        if (g_have_counts || !g_hle) {
                            if (g_jobprof_rec) {
                                g_cur_job->hle_cyc += hdt;
                                g_cur_job->hle_insns += k;
                                g_cur_job->hle_calls++;
                                g_cur_job->disp_cyc += h0 - jp_top;
                            }
                            g_hle_tsc += hdt;
                            if (g_hle_profile) {
                                HleStat& hs = g_hle_stats[f->name];
                                hs.cycles += hdt;
                                hs.insns += k;
                                hs.calls++;
                            }
                        }
                        g_hle_calls++;
                        g_hle_insns += k;
                        s.c.irq_check = false;
                        if (host.pos < cap.events.size() && cap.events[host.pos].kind == 5 && cap.events[host.pos].value == n) host.next(5, 0, uint32_t(n), true);
                        return true;
                    }
                    s.c.pc = pc0;
                }
                return false;
            };
            if (f && f->host_handler) {
                if (try_host(f)) continue;
                f = nullptr;
            }
            if (f) {
                ULONG64 x0 = 0, x1 = 0;
                if (!g_have_counts) QueryThreadCycleTime(GetCurrentThread(), &x0);
                uint64_t k = 0;
                bool iso_ok;
                uint32_t exit_pc = 0;
                if (g_have_counts) {
                    k = g_count_pos < g_counts.size() ? g_counts[g_count_pos++] : UINT64_MAX;
                    iso_ok = k != UINT64_MAX;
                } else {
                    static std::vector<uint8_t> probe_ls;
                    probe_ls.assign(s.ls, s.ls + spu::LS_SIZE + 16);
                    spu::Context probe = s.c;
                    probe.ls = probe_ls.data();
                    probe.line_epoch = nullptr;
                    void* gs = edge::gpu::mode() ? edge::gpu::state_save() : nullptr;
                    iso_ok = f->handler(probe);
                    if (gs) edge::gpu::state_restore(gs);
                    exit_pc = probe.gpr[0].u32[3] & spu::PC_MASK;
                    if (iso_ok) iso_ok = iso_count(s, k, exit_pc, probe.gpr[1].u32[3]);
                    g_counts.push_back(iso_ok && n + k < cap.insns ? k : UINT64_MAX);
                }
                if (!g_have_counts) QueryThreadCycleTime(GetCurrentThread(), &x1);
                excluded += x1 - x0;
                const spu::hle::Function* fb = nullptr;
                if (!iso_ok) {
                    fb = spu::hle::lookup_next(s.ls, s.c.pc, f);
                    if (fb && !fb->host_handler) fb = nullptr;
                }
                if (fb && try_host(fb)) continue;
                if (iso_ok && n + k < cap.insns) {
                    if (g_js_on) g_phase = js_base | PH_NATIVE;
                    uint64_t h0 = __rdtsc();
                    bool handled = f->handler(s.c);
                    if (g_js_on) g_phase = js_base | PH_DISP;
                    uint64_t hdt = __rdtsc() - h0;
                    if (g_have_counts) {
                        if (g_jobprof_rec && handled) {
                            g_cur_job->hle_cyc += hdt;
                            g_cur_job->hle_insns += k;
                            g_cur_job->hle_calls++;
                            g_cur_job->disp_cyc += h0 - jp_top;
                        }
                        g_hle_tsc += hdt;
                        if (g_hle_profile && handled) {
                            HleStat& hs = g_hle_stats[f->name];
                            hs.cycles += hdt;
                            hs.insns += k;
                            hs.calls++;
                        }
                    }
                    if (handled) {
                        if (!g_hle_strict && !g_have_counts) {
                            spu::v128 r3 = s.c.gpr[3];
                            std::memcpy(s.c.gpr, g_iso.gpr, sizeof s.c.gpr);
                            if (std::memcmp(&r3, &g_iso.gpr[3], 16) != 0 && host.error.empty()) {
                                char b[160];
                                std::snprintf(b, sizeof b, "hle r3 differs in %s (native %08x %08x %08x %08x, spu %08x %08x %08x %08x)", f->name, r3.u32[3], r3.u32[2], r3.u32[1], r3.u32[0], g_iso.gpr[3].u32[3], g_iso.gpr[3].u32[2], g_iso.gpr[3].u32[1], g_iso.gpr[3].u32[0]);
                                host.error = b;
                            }
                        }
                        s.c.pc = g_have_counts ? s.c.gpr[0].u32[3] & spu::PC_MASK : exit_pc;
                        uint64_t target = n + k;
                        while (host.pos < cap.events.size() && cap.events[host.pos].kind == 5 && cap.events[host.pos].value <= target) host.apply_lines(cap.events[host.pos++]);
                        n = target;
                        g_hle_calls++;
                        g_hle_insns += k;
                        continue;
                    }
                }
            }
        }
        if (g_watch_log && std::find(g_watch.begin(), g_watch.end(), s.c.pc) != g_watch.end()) watch_hit(s.c);
        Slot& slot = cache[s.c.pc >> 2];
        const spu::CompiledBlock* b = slot.b;
        if (!b || !s.fresh(s.c.pc, b->words, slot.valid)) {
            uint32_t e = s.epoch.load();
            if (!b || std::memcmp(s.ls + s.c.pc, b->code, b->words * 4) != 0) b = spu::aot::find(s.ls, s.c.pc);
            slot.b = b;
            slot.valid = b ? e : 0;
        }
        static const uint32_t noaot_lo = std::getenv("GOWA_BENCH_NOAOT_EDGE") ? 0x10c80u : 0u, noaot_hi = noaot_lo ? 0x29000u : 0u;
        if (b && s.c.pc >= noaot_lo && s.c.pc < noaot_hi && edge::image_loaded(s.ls)) b = nullptr;
        r.dispatches++;
        if (!b) {
            uint64_t i0 = g_jobprof_rec ? __rdtsc() : 0;
            if (g_jobprof_rec) g_cur_job->disp_cyc += i0 - jp_top;
            if (g_js_on) g_phase = js_base | (PH_KERNEL + region_of(s.c.pc));
            spu::step(s.c, host);
            if (g_js_on) g_phase = js_base | PH_DISP;
            if (g_jobprof_rec) {
                g_cur_job->interp_cyc[g_cur_region] += __rdtsc() - i0;
                g_cur_job->interp_insns[g_cur_region]++;
            }
            n++;
            r.interpreted++;
            if (host.pos < cap.events.size() && cap.events[host.pos].kind == 5 && cap.events[host.pos].value == n) host.next(5, 0, uint32_t(n), true);
            continue;
        }
        uint32_t ran = b->words;
        bool ok;
        uint64_t b0 = g_profile || g_jobprof_rec ? __rdtsc() : 0;
        uint64_t host0 = 0;
        if (g_jobprof_rec) {
            g_cur_job->disp_cyc += b0 - jp_top;
            host0 = g_cur_job->host_cyc[g_cur_region];
        }
        if (g_js_on) g_phase = js_base | (PH_KERNEL + region_of(s.c.pc));
        if (regions && b->region && (to_last || cap.insns - n > 1000000)) {
            s.c.executed = 0;
            ok = b->region(s.c, host);
            if (s.c.executed) ran = s.c.executed;
            else ok = b->fn(s.c, host);
        } else {
            ok = b->fn(s.c, host);
        }
        n += ran;
        if (g_js_on) g_phase = js_base | PH_DISP;
        if (g_jobprof_rec) {
            uint64_t dt = __rdtsc() - b0;
            JobAcc& a = *g_cur_job;
            uint64_t hd = a.host_cyc[g_cur_region] - host0;
            a.block_cyc[g_cur_region] += dt > hd ? dt - hd : 0;
            a.block_insns[g_cur_region] += ran;
            a.blocks[g_cur_region]++;
            BlockStatJ& st = g_job_blocks[{g_cur_key, b->lsa}];
            st.cycles += dt > hd ? dt - hd : 0;
            st.insns += ran;
            st.calls++;
        }
        if (g_profile) {
            BlockStat& st = g_stats[b];
            st.cycles += __rdtsc() - b0;
            st.insns += ran;
            st.calls++;
        }
        s.c.irq_check = false;
        if (!ok) break;
        if (host.pos < cap.events.size() && cap.events[host.pos].kind == 5 && cap.events[host.pos].value == n) host.next(5, 0, uint32_t(n), true);
    }
    ULONG64 t1 = 0;
    QueryThreadCycleTime(GetCurrentThread(), &t1);
    if (g_js_on) {
        g_js_active.store(false);
        g_phase = PH_IDLE;
    }
    r.cycles = t1 - t0 - excluded;
    r.insns = n;
    r.error = host.error;
    if (!g_state_out.empty()) {
        if (std::FILE* f = std::fopen(g_state_out.c_str(), "wb")) {
            std::fwrite(s.c.gpr, 16, 128, f);
            std::fwrite(s.ls, 1, spu::LS_SIZE, f);
            std::fclose(f);
        }
    }
    if (to_last) {
        r.exact = r.error.empty();
        if (r.exact) r.error = "stopped at last event";
        return r;
    }
    if (r.error.empty() && host.pos != cap.events.size()) r.error = "consumed " + std::to_string(host.pos) + " of " + std::to_string(cap.events.size()) + " events";
    if (r.error.empty() && n != cap.insns) r.error = "executed " + std::to_string(n) + " of " + std::to_string(cap.insns) + " insns";
    if (r.error.empty()) {
        std::string diff;
        if (s.c.pc != cap.pc1) diff += " pc " + std::to_string(s.c.pc) + "!=" + std::to_string(cap.pc1);
        for (int i = 0; i < 128; i++) {
            if (std::memcmp(&s.c.gpr[i], cap.gpr1 + i * 16, 16) != 0) {
                diff += " r" + std::to_string(i);
                if (diff.size() > 120) break;
            }
        }
        uint32_t bad_lines = 0;
        for (uint32_t l = 0; l < spu::LS_SIZE; l += 128) bad_lines += std::memcmp(s.ls + l, cap.ls1 + l, 128) != 0;
        if (bad_lines) diff += " ls lines " + std::to_string(bad_lines);
        r.exact = diff.empty();
        if (!diff.empty()) r.error = "final state differs:" + diff;
    }
    return r;
}

}

int main(int argc, char** argv) {
    _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x6000u | 0x8040u);
    if (argc < 2) {
        std::fprintf(stderr, "usage: spu_bench [--reps N] [--no-regions] capture.bin...\n");
        return 2;
    }
    int reps = 5;
    bool regions = true;
    bool live = false;
    int live_mode = 3;
    std::string live_what = "direct";
    std::vector<std::string> files;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--reps" && i + 1 < argc) reps = std::atoi(argv[++i]);
        else if (a == "--no-regions") regions = false;
        else if (a == "--profile") g_profile = true;
        else if (a == "--sample") g_sample = true;
        else if (a == "--no-skip-same") g_skip_same = false;
        else if (a == "--hle") g_hle = true;
        else if (a == "--hle-strict") g_hle = g_hle_strict = true;
        else if (a == "--hle-profile") g_hle = g_hle_profile = true;
        else if (a == "--jobprof") g_jobprof = true;
        else if (a == "--live") live = true;
        else if (a == "--live-what" && i + 1 < argc) live_what = argv[++i];
        else if (a == "--live-mode" && i + 1 < argc) live_mode = std::atoi(argv[++i]);
        else if (a == "--jobsample") g_jobsample = true;
        else if (a == "--pmtrace" && i + 1 < argc) g_pmtrace = std::fopen(argv[++i], "w");
        else if (a == "--to-last-event") g_to_last_event = true;
        else if (a == "--state-out" && i + 1 < argc) g_state_out = argv[++i];
        else if (a == "--dma-log" && i + 1 < argc) g_dma_log = std::fopen(argv[++i], "w");
        else if (a == "--watch" && i + 2 < argc) {
            std::string list = argv[++i];
            g_watch_log = std::fopen(argv[++i], "w");
            size_t pos = 0;
            while (pos < list.size()) {
                size_t e = list.find(',', pos);
                if (e == std::string::npos) e = list.size();
                g_watch.push_back(static_cast<uint32_t>(std::strtoul(list.substr(pos, e - pos).c_str(), nullptr, 16)));
                pos = e + 1;
            }
        }
        else files.push_back(a);
    }
    if (g_hle) spu::hle::install_builtin();
    State s;
    std::thread js_thread;
    if (g_jobsample) {
        g_js_run = true;
        js_thread = std::thread(js_sampler, 100000);
    }
    std::thread sampler_thread;
    if (g_sample) {
        HANDLE self = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
        g_sampling = true;
        sampler_thread = std::thread(sampler, self);
    }
#ifdef SPU_BENCH_LIVE
    if (live) {
        vm::init();
        g_hle = true;
        g_skip_same = false;
        int bad = 0;
        int live_reps = reps == 5 ? 2 : reps;
        uint64_t live_total[2] = {};
        for (const std::string& f : files) {
            Capture cap;
            if (!load(f, cap)) {
                std::printf("%s: bad capture\n", f.c_str());
                bad++;
                continue;
            }
            g_counts.clear();
            g_have_counts = false;
            std::map<uint32_t, std::vector<uint8_t>> mem[2];
            std::vector<uint8_t> ls[2];
            std::vector<uint8_t> gpr[2];
            uint32_t pc[2] = {};
            uint64_t cyc[2] = {};
            std::string err[2];
            for (int mode = 0; mode < 2; mode++) {
                for (uint32_t b : g_live_blocks) std::memset(vm::ptr(b), 0, 0x10000);
                auto t = std::make_unique<spu::SpuThread>();
                uint8_t* own = t->ls;
                t->ls = s.ls;
                g_live_thread = t.get();
                spu::g_edge_direct = live_what == "direct" && mode ? live_mode : 0;
                if (live_what == "ovl") spu::hle::g_dyn_on = mode != 0;
                uint64_t hits0 = spu::g_edge_merge_hits.load();
                g_live_mismatch = 0;
                g_live_first.clear();
                g_count_pos = 0;
                Result r = run(s, cap, regions);
                g_have_counts = true;
                cyc[mode] = r.cycles;
                char b[64];
                std::snprintf(b, sizeof b, "%llu live mismatches", static_cast<unsigned long long>(g_live_mismatch));
                err[mode] = (r.exact ? std::string("exact") : r.error) + ", " + b + (g_live_first.empty() ? "" : " (first: " + g_live_first + ")");
                char st[160];
                std::snprintf(st, sizeof st, "; edge merge hits %llu, lockless ring puts %llu, unsnapshotted gets %llu", static_cast<unsigned long long>(spu::g_edge_merge_hits.load() - hits0),
                              static_cast<unsigned long long>(t->edge_direct_puts), static_cast<unsigned long long>(t->edge_nosnap_gets));
                err[mode] += st;
                for (uint32_t blk : g_live_blocks) mem[mode][blk].assign(vm::ptr(blk), vm::ptr(blk) + 0x10000);
                ls[mode].assign(s.ls, s.ls + spu::LS_SIZE);
                gpr[mode].assign(reinterpret_cast<const uint8_t*>(s.c.gpr), reinterpret_cast<const uint8_t*>(s.c.gpr) + 2048);
                pc[mode] = s.c.pc;
                t->ls = own;
                g_live_thread = nullptr;
            }
            g_live_check = false;
            if (live_reps) cyc[0] = cyc[1] = UINT64_MAX;
            for (int k = 0; k < 2 * live_reps; k++) {
                int mode = k & 1;
                auto t = std::make_unique<spu::SpuThread>();
                uint8_t* own = t->ls;
                t->ls = s.ls;
                g_live_thread = t.get();
                spu::g_edge_direct = live_what == "direct" && mode ? live_mode : 0;
                if (live_what == "ovl") spu::hle::g_dyn_on = mode != 0;
                g_count_pos = 0;
                Result r = run(s, cap, regions);
                if (std::getenv("SPU_LIVE_VERBOSE")) std::printf("  rep %d mode %d cycles %llu\n", k, mode, static_cast<unsigned long long>(r.cycles));
                if (r.cycles < cyc[mode]) cyc[mode] = r.cycles;
                t->ls = own;
                g_live_thread = nullptr;
            }
            g_live_check = true;
            live_total[0] += cyc[0];
            live_total[1] += cyc[1];
            uint64_t mem_diff = 0;
            uint32_t first_diff = 0;
            std::set<uint32_t> keys;
            for (auto& [k, v] : mem[0]) keys.insert(k);
            for (auto& [k, v] : mem[1]) keys.insert(k);
            static const std::vector<uint8_t> zero(0x10000, 0);
            for (uint32_t k : keys) {
                const std::vector<uint8_t>& a = mem[0].count(k) ? mem[0][k] : zero;
                const std::vector<uint8_t>& b = mem[1].count(k) ? mem[1][k] : zero;
                for (uint32_t i = 0; i < 0x10000; i++)
                    if (a[i] != b[i]) {
                        if (!mem_diff) first_diff = k + i;
                        mem_diff++;
                    }
            }
            uint32_t ls_diff = 0;
            for (uint32_t l = 0; l < spu::LS_SIZE; l += 128) ls_diff += std::memcmp(ls[0].data() + l, ls[1].data() + l, 128) != 0;
            bool regs_same = gpr[0] == gpr[1] && pc[0] == pc[1];
            bool same = !mem_diff && !ls_diff && regs_same;
            std::printf("%-28s ref: %s\n%-28s direct: %s\n%-28s memory effects %s: %zu blocks, %llu bytes differ%s, %u ls lines differ, regs %s; best cycles ref %llu direct %llu\n", f.c_str(), err[0].c_str(), "", err[1].c_str(), "", same ? "EQUAL" : "DIFFER", keys.size(),
                        static_cast<unsigned long long>(mem_diff), mem_diff ? (" first at " + std::to_string(first_diff)).c_str() : "", ls_diff, regs_same ? "equal" : "differ", static_cast<unsigned long long>(cyc[0]), static_cast<unsigned long long>(cyc[1]));
            if (!same) bad++;
        }
        std::printf("live: %d captures with differing memory effects, total cycles ref %llu direct %llu (%+.2f%%)\n", bad, static_cast<unsigned long long>(live_total[0]), static_cast<unsigned long long>(live_total[1]),
                    live_total[0] ? 100.0 * (double(live_total[1]) - double(live_total[0])) / double(live_total[0]) : 0.0);
        if (g_sample) {
            g_sampling = false;
            sampler_thread.join();
            report_samples();
        }
        std::fflush(stdout);
        std::_Exit(bad ? 1 : 0);
    }
#endif
    uint64_t total_cycles = 0, total_insns = 0;
    int failures = 0;
    for (const std::string& f : files) {
        Capture cap;
        if (!load(f, cap)) {
            std::printf("%s: bad capture\n", f.c_str());
            failures++;
            continue;
        }
        Result best;
        g_counts.clear();
        g_have_counts = false;
        std::map<JobKey, JobAcc> best_jobs;
        JobBlocks best_blocks;
        for (int k = 0; k < reps; k++) {
            g_count_pos = 0;
            g_jobs.clear();
            g_job_blocks.clear();
            Result r = run(s, cap, regions);
            if (g_jobprof_rec && (!best.insns || r.cycles < best.cycles)) {
                best_jobs.swap(g_jobs);
                best_blocks.swap(g_job_blocks);
            }
            if (g_hle && k == 0) {
                std::printf("%-28s hle verify run: %s\n", f.c_str(), r.exact ? "exact" : r.error.c_str());
                if (!r.exact) failures++;
                g_have_counts = true;
                if (reps > 1) continue;
            }
            if (!best.insns || r.cycles < best.cycles) best = r;
        }
        std::printf("%-28s slot %u insns %9llu events %6zu  %6.2f cycles/insn  %5.1f insns/dispatch  interp %llu  %s\n", f.c_str(), cap.slot, static_cast<unsigned long long>(best.insns),
                    cap.events.size(), best.insns ? double(best.cycles) / best.insns : 0.0, best.dispatches ? double(best.insns) / best.dispatches : 0.0,
                    static_cast<unsigned long long>(best.interpreted), best.exact ? "exact" : best.error.c_str());
        if (!best.exact) failures++;
        if (g_jobprof) jobprof_merge(best_jobs, best_blocks);
        total_cycles += best.cycles;
        total_insns += best.insns;
    }
    if (g_hle) std::printf("hle: %llu native calls replaced %llu SPU insns (all reps), %llu TSC cycles in handlers (timing reps)\n", static_cast<unsigned long long>(g_hle_calls), static_cast<unsigned long long>(g_hle_insns), static_cast<unsigned long long>(g_hle_tsc));
    if (g_hle_profile) {
        std::vector<std::pair<std::string, HleStat>> v(g_hle_stats.begin(), g_hle_stats.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.cycles > b.second.cycles; });
        for (auto& [name, st] : v)
            std::printf("  %-28s calls %8llu insns %10llu cycles %11llu  %6.2f cycles/insn %8.0f cycles/call\n", name.c_str(), static_cast<unsigned long long>(st.calls), static_cast<unsigned long long>(st.insns),
                        static_cast<unsigned long long>(st.cycles), st.insns ? double(st.cycles) / st.insns : 0.0, st.calls ? double(st.cycles) / st.calls : 0.0);
    }
    if (g_sample) {
        g_sampling = false;
        sampler_thread.join();
        report_samples();
    }
    if (g_profile) {
        if (std::FILE* f = std::fopen("spu_bench_blocks.txt", "w")) {
            for (auto& [b, st] : g_stats) std::fprintf(f, "%u %u %llu %llu %llu\n", b->lsa, b->words, static_cast<unsigned long long>(st.calls), static_cast<unsigned long long>(st.insns), static_cast<unsigned long long>(st.cycles));
            std::fclose(f);
        }
        std::vector<std::pair<const spu::CompiledBlock*, BlockStat>> v(g_stats.begin(), g_stats.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.cycles > b.second.cycles; });
        uint64_t tot = 0;
        for (auto& e : v) tot += e.second.cycles;
        size_t count = 0;
        const spu::CompiledBlock* all = spu::compiled_blocks(count);
        for (size_t i = 0; i < v.size() && i < 40; i++) {
            const BlockStat& st = v[i].second;
            std::printf("  b%-6zu lsa %05x words %3u region %d: %5.1f%% cycles, %6.2f cycles/insn, %7.1f insns/call\n", static_cast<size_t>(v[i].first - all), v[i].first->lsa, v[i].first->words,
                        v[i].first->region ? 1 : 0, 100.0 * st.cycles / tot, double(st.cycles) / st.insns, double(st.insns) / st.calls);
        }
    }
    if (g_jobsample) {
        g_js_run = false;
        js_thread.join();
        if (std::FILE* f = std::fopen("spu_bench_jobsample.txt", "w")) {
            for (size_t i = 0; i < g_js_keys.size(); i++) {
                std::fprintf(f, "key %zu ", i);
                for (int b = 0; b < 16; b++) std::fprintf(f, "%02x", g_js_keys[i].img[b]);
                std::fprintf(f, " %08x %u\n", g_js_keys[i].pm, g_js_keys[i].wid);
            }
            for (auto& [ph, c] : g_js_hist) std::fprintf(f, "hist %u %u %llu\n", ph >> 4, ph & 15, (unsigned long long)c);
            std::fclose(f);
        }
    }
    if (g_jobprof) {
        if (std::FILE* f = std::fopen("spu_bench_jobs.txt", "w")) {
            for (auto& [k, a] : g_jobs_total) {
                std::fprintf(f, "job ");
                for (int i = 0; i < 16; i++) std::fprintf(f, "%02x", k.img[i]);
                std::fprintf(f, " pm %08x wid %u entries %llu disp %llu disp_cyc %llu hle %llu %llu %llu\n", k.pm, k.wid, (unsigned long long)a.entries, (unsigned long long)a.disp, (unsigned long long)a.disp_cyc,
                             (unsigned long long)a.hle_calls, (unsigned long long)a.hle_insns, (unsigned long long)a.hle_cyc);
                for (int r = 0; r < R_COUNT; r++) {
                    std::fprintf(f, "  r%d blocks %llu insns %llu cyc %llu host %llu interp %llu %llu spin %llu", r, (unsigned long long)a.blocks[r], (unsigned long long)a.block_insns[r], (unsigned long long)a.block_cyc[r],
                                 (unsigned long long)a.host_cyc[r], (unsigned long long)a.interp_insns[r], (unsigned long long)a.interp_cyc[r], (unsigned long long)a.spin_getllar[r]);
                    std::fprintf(f, " chan");
                    for (int k = 0; k < 4; k++)
                        for (int c = 0; c < 32; c++)
                            if (a.chan[r][k][c]) std::fprintf(f, " %c%d=%llu", "rwcs"[k], c, (unsigned long long)a.chan[r][k][c]);
                    std::fprintf(f, " mfc");
                    for (int c = 0; c < 256; c++)
                        if (a.mfc_cmd[r][c]) std::fprintf(f, " %02x=%llu/%llu", c, (unsigned long long)a.mfc_cmd[r][c], (unsigned long long)a.mfc_bytes[r][c]);
                    std::fprintf(f, "\n");
                }
            }
            for (auto& [kk, st] : g_job_blocks_total) {
                std::fprintf(f, "block ");
                for (int i = 0; i < 16; i++) std::fprintf(f, "%02x", kk.first.img[i]);
                std::fprintf(f, " %08x %u %05x %llu %llu %llu\n", kk.first.pm, kk.first.wid, kk.second, (unsigned long long)st.calls, (unsigned long long)st.insns, (unsigned long long)st.cycles);
            }
            std::fclose(f);
        }
    }
    if (g_exit_hist) {
        std::map<uint32_t, uint64_t> by_exit;
        for (auto& [k, v] : *g_exit_hist) by_exit[k.second] += v;
        std::vector<std::pair<uint64_t, uint32_t>> v;
        for (auto& [pc, c] : by_exit) v.push_back({c, pc});
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < v.size() && i < 40; i++) std::printf("  exit %05x %llu\n", v[i].second, (unsigned long long)v[i].first);
    }
    if (edge::gpu::mode()) {
        edge::gpu::Stats st = edge::gpu::stats();
        std::printf("edge gpu: deferred %llu confirmed %llu published %llu dropped %llu materialized", (unsigned long long)st.deferred, (unsigned long long)st.confirmed, (unsigned long long)st.published, (unsigned long long)st.dropped);
        for (int i = 0; i < 8; i++) std::printf(" %llu", (unsigned long long)st.materialized[i]);
        std::printf("\n");
    }
    std::printf("total %.3f cycles/insn over %llu insns, %d mismatching\n", total_insns ? double(total_cycles) / total_insns : 0.0, static_cast<unsigned long long>(total_insns), failures);
    return failures ? 1 : 0;
}
