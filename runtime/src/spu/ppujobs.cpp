#include "runtime/ppujobs.h"

#include "hle.h"
#include "thread.h"

#include "ppu/context.h"
#include "runtime/critpath.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/reservation.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <immintrin.h>

namespace crash {
void profile_thread(void* native, const char* name);
}

namespace ppujobs {

namespace {

constexpr uint32_t kJobRet = 0x3368;
constexpr uint32_t kLoadNextJob = 0x17e0;
constexpr uint32_t kPmEa = 0x7a7b00;
constexpr uint32_t kPmSize = 0x3000;
constexpr uint32_t kPmLs = 0xa00;
constexpr uint32_t kCmdLs = 0xc00;
constexpr uint32_t kApiLs = 0x1340;
constexpr uint32_t kCodeLs = 0x4000;
constexpr uint32_t kMaxDeps = 4;
constexpr uint32_t kMaxLists = 16;

struct Config {
    bool on = false;
    int threads = 2;
    bool help = true;
    uint32_t verify = 0;
    std::vector<uint32_t> mods;
    std::vector<int> cpus;
    uint32_t poison = 0;
};

std::vector<uint32_t> parse_hex_list(const char* e) {
    std::vector<uint32_t> v;
    if (!e) return v;
    std::string s = e;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        if (end > pos) v.push_back(static_cast<uint32_t>(std::strtoul(s.substr(pos, end - pos).c_str(), nullptr, 16)));
        pos = end + 1;
    }
    return v;
}

const Config& cfg() {
    static const Config c = [] {
        Config c;
        const char* e = std::getenv("GOWA_PPU_JOBS");
        c.on = e && e[0] && e[0] != '0';
        if (const char* t = std::getenv("GOWA_PPU_JOBS_THREADS")) c.threads = std::clamp(std::atoi(t), 0, 16);
        if (const char* h = std::getenv("GOWA_PPU_JOBS_HELP")) c.help = h[0] != '0';
        if (const char* v = std::getenv("GOWA_PPU_JOBS_VERIFY")) c.verify = static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
        c.mods = parse_hex_list(std::getenv("GOWA_PPU_JOBS_MODS"));
        if (c.mods.empty()) c.mods.push_back(0x705680);
        for (uint32_t x : parse_hex_list(std::getenv("GOWA_PPU_JOBS_CPUS"))) c.cpus.push_back(static_cast<int>(x));
        if (const char* p = std::getenv("GOWA_PPU_JOBS_POISON")) c.poison = static_cast<uint32_t>(std::strtoul(p, nullptr, 16));
        if (!c.help && c.threads == 0) c.threads = 1;
        return c;
    }();
    return c;
}

struct ListState {
    std::atomic<uint32_t> obj{0};
    std::atomic<int32_t> pending{0};
};

ListState g_lists[kMaxLists];

ListState* list_state(uint32_t obj, bool create) {
    for (ListState& l : g_lists) {
        if (l.obj.load(std::memory_order_acquire) == obj) return &l;
    }
    if (!create) return nullptr;
    static std::mutex m;
    std::lock_guard lk(m);
    for (ListState& l : g_lists) {
        if (l.obj.load(std::memory_order_acquire) == obj) return &l;
    }
    for (ListState& l : g_lists) {
        uint32_t zero = 0;
        if (l.obj.compare_exchange_strong(zero, obj)) return &l;
    }
    return nullptr;
}

struct Job {
    uint64_t header = 0;
    uint32_t cmd_ea = 0;
    uint32_t cmd_size = 0;
    uint32_t list_obj = 0;
    uint32_t list_hdr = 0;
    uint32_t wid = 0;
    uint32_t spurs = 0;
    uint32_t code_ea = 0;
    uint32_t code_size = 0;
    uint32_t param_off = 0;
    uint32_t deps[kMaxDeps] = {};
    uint32_t ndeps = 0;
    ListState* ls = nullptr;
    uint64_t t_submit = 0;
};

bool parse(Job& j) {
    if (j.cmd_size < 0x20 || j.cmd_size > 0x100 || (j.cmd_ea & 15) || !vm::range_mapped(j.cmd_ea, j.cmd_size)) return false;
    bool reserved = false;
    for (uint32_t off = 0; off + 8 <= j.cmd_size; off += 8) {
        uint64_t c = vm::read64(j.cmd_ea + off);
        if (!c) continue;
        uint32_t hi = static_cast<uint32_t>(c >> 32), lo = static_cast<uint32_t>(c);
        uint32_t op = (hi >> 9) & 7, set = (hi >> 5) & 15;
        switch (op) {
        case 1:
            if (set != 0 || (lo >> 24) != 0x10 || ((lo >> 8) & 0xff) != 1 || reserved) return false;
            reserved = true;
            break;
        case 2: {
            if (set != 0 || (hi & 0x1f) || (hi & 0x10000) || !reserved || j.code_ea) return false;
            uint32_t len = (hi >> 18) << 4;
            if (!len || len > 0x30000 || (lo & 15) || !vm::range_mapped(lo, len)) return false;
            j.code_ea = lo;
            j.code_size = len;
            break;
        }
        case 4:
            if (j.ndeps >= kMaxDeps || !lo || !vm::range_mapped(lo & ~127u, 128)) return false;
            j.deps[j.ndeps++] = lo;
            break;
        case 5:
            if (!j.code_ea) return false;
            j.param_off = (off + 8 + 15) & ~15u;
            if (j.param_off > j.cmd_size) return false;
            if (vm::read32(j.code_ea + 0x20) != 0xc0dec0de) return false;
            return true;
        default:
            return false;
        }
    }
    return false;
}

struct Stats {
    std::atomic<uint64_t> jobs{0}, helped{0}, cycles{0}, cpu{0}, insns{0}, waits{0}, wait_cycles{0}, captured{0}, fallback{0}, verified{0}, mismatches{0};
};
Stats g_stats;

std::mutex g_qm;
std::condition_variable g_qcv;
std::deque<Job> g_queue;
std::atomic<uint32_t> g_queued{0};

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

struct Kick {
    uint32_t wid = 0;
    uint8_t value = 0;
    bool fire = false;
};

void kick_workload(uint32_t spurs, uint32_t wid, uint8_t value) {
    struct K {
        uint32_t wid;
        uint8_t value;
    } k{wid, value};
    update_line(spurs + (wid < 16 ? 0 : 0x10) + (wid & 15), [](uint8_t* l, uint32_t off, void* u) {
        auto* k = static_cast<K*>(u);
        l[off] = k->value;
        return true;
    }, &k);
}

void decrement(uint32_t ea, uint32_t spurs) {
    Kick k;
    update_line(ea, [](uint8_t* l, uint32_t off, void* u) {
        auto* k = static_cast<Kick*>(u);
        uint16_t cnt = uint16_t((l[off] << 8) | l[off + 1]);
        uint16_t nc = uint16_t(cnt - 1);
        l[off] = uint8_t(nc >> 8);
        l[off + 1] = uint8_t(nc);
        k->fire = cnt == 1;
        k->wid = l[off + 2];
        k->value = l[off + 3];
        return true;
    }, &k);
    if (k.fire && k.value) kick_workload(spurs, k.wid, k.value);
}

struct Exec;

struct ChanRec {
    int kind;
    uint32_t ch;
    uint32_t value;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> lines;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> outs;
};

struct Recorder : spu::Host {
    spu::SpuThread& t;
    std::vector<ChanRec> ev;
    uint32_t lsa = 0, eal = 0, size = 0;
    explicit Recorder(spu::SpuThread& th) : t(th) {}
    uint32_t read_channel(uint32_t ch) override {
        uint32_t v = t.read_channel(ch);
        ev.push_back({0, ch, v, {}, {}});
        return v;
    }
    void write_channel(uint32_t ch, uint32_t value) override {
        if (ch == 16) lsa = value;
        else if (ch == 18) eal = value;
        else if (ch == 19) size = value;
        t.write_channel(ch, value);
        ChanRec r{1, ch, value, {}, {}};
        if (ch == 21) {
            uint32_t cmd = value & 0xff;
            auto grab = [&](uint32_t a, uint32_t n) {
                a &= 0x3ffff;
                if (n > 0x40000 - a) n = 0x40000 - a;
                r.lines.push_back({a, std::vector<uint8_t>(t.ls + a, t.ls + a + n)});
            };
            auto out = [&](uint32_t a, uint32_t n) {
                a &= 0x3ffff;
                if (n > 0x40000 - a) n = 0x40000 - a;
                r.outs.push_back({a, std::vector<uint8_t>(t.ls + a, t.ls + a + n)});
            };
            bool put = (cmd >= 0x20 && cmd <= 0x26) || (cmd >= 0x30 && cmd <= 0x36) || cmd == 0xa0 || cmd == 0xa1 || cmd == 0xa2;
            if (put && ((cmd & 0xf) >= 4 && (cmd & 0xf) <= 6)) {
                uint32_t cur = lsa & 0x3fff0, list = eal & 0x3fff8;
                for (uint32_t n = size; n >= 8; n -= 8) {
                    uint8_t* e = t.ls + list;
                    uint32_t ts = ((uint32_t(e[2]) << 8) | e[3]) & 0x7fff;
                    uint32_t ea;
                    std::memcpy(&ea, e + 4, 4);
                    ea = __builtin_bswap32(ea);
                    out(cur | (ea & 15), ts);
                    cur = (cur + ((ts + 15) & ~15u)) & 0x3fff0;
                    list = (list + 8) & 0x3fff8;
                }
            } else if (put) {
                out(lsa, cmd >= 0xa0 ? 4 : size);
            } else if (cmd == 0xb4 || cmd == 0xb0 || cmd == 0xb8) {
                out(lsa & ~127u, 128);
            }
            if (cmd == 0x40 || cmd == 0x41 || cmd == 0x42) grab(lsa, size);
            else if (cmd == 0xd0) grab(lsa & ~127u, 128);
            else if (cmd == 0x44 || cmd == 0x45 || cmd == 0x46) {
                uint32_t cur = lsa & 0x3fff0, list = eal & 0x3fff8;
                for (uint32_t n = size; n >= 8; n -= 8) {
                    uint8_t* e = t.ls + list;
                    uint32_t ts = ((uint32_t(e[2]) << 8) | e[3]) & 0x7fff;
                    uint32_t ea;
                    std::memcpy(&ea, e + 4, 4);
                    ea = __builtin_bswap32(ea);
                    grab(cur | (ea & 15), ts);
                    cur = (cur + ((ts + 15) & ~15u)) & 0x3fff0;
                    list = (list + 8) & 0x3fff8;
                }
            }
        }
        ev.push_back(std::move(r));
    }
    uint32_t channel_count(uint32_t ch) override {
        uint32_t v = t.channel_count(ch);
        ev.push_back({2, ch, v, {}, {}});
        return v;
    }
    bool stop_and_signal(uint32_t code) override {
        bool r = t.stop_and_signal(code);
        ev.push_back({3, code, 0, {}, {}});
        return r;
    }
    void halt(uint32_t pc, uint32_t word) override {
        t.halt(pc, word);
        ev.push_back({4, pc, word, {}, {}});
    }
};

struct Replayer : spu::Host {
    spu::SpuThread& t;
    const std::vector<ChanRec>& ev;
    const spu::SpuThread& ref;
    size_t pos = 0;
    std::string error;
    uint32_t puts = 0, put_bytes = 0;
    uint32_t lsa = 0, eal = 0, size = 0;
    Replayer(spu::SpuThread& th, const std::vector<ChanRec>& e, const spu::SpuThread& r) : t(th), ev(e), ref(r) {}
    const ChanRec* next(int kind, uint32_t ch, uint32_t value, bool check) {
        if (pos >= ev.size()) {
            if (error.empty()) error = "extra channel op kind " + std::to_string(kind) + " ch " + std::to_string(ch) + " pc " + std::to_string(t.ctx.pc);
            t.stop_flag.store(true);
            t.ctx.halted = true;
            return nullptr;
        }
        const ChanRec& e = ev[pos++];
        if (e.kind != kind || e.ch != ch || (check && e.value != value)) {
            if (error.empty()) {
                char b[200];
                std::snprintf(b, sizeof b, "op %zu: got kind %d ch %u val 0x%x, ref kind %d ch %u val 0x%x at pc 0x%05x", pos - 1, kind, ch, value, e.kind, e.ch, e.value, t.ctx.pc);
                error = b;
            }
            t.stop_flag.store(true);
            t.ctx.halted = true;
            return nullptr;
        }
        return &e;
    }
    void compare_out(uint32_t a, uint32_t n, const uint8_t* expect) {
        a &= 0x3ffff;
        if (n > 0x40000 - a) n = 0x40000 - a;
        puts++;
        put_bytes += n;
        if (std::memcmp(t.ls + a, expect, n) != 0 && error.empty()) {
            uint32_t k = 0;
            while (k < n && t.ls[a + k] == expect[k]) k++;
            char b[160];
            std::snprintf(b, sizeof b, "output differs: lsa 0x%05x size 0x%x first diff at +0x%x (0x%02x vs ref 0x%02x) ea 0x%08x pc 0x%05x", a, n, k, t.ls[a + k], expect[k], eal, t.ctx.pc);
            error = b;
        }
    }
    uint32_t read_channel(uint32_t ch) override {
        const ChanRec* e = next(0, ch, 0, false);
        return e ? e->value : 0;
    }
    void write_channel(uint32_t ch, uint32_t value) override {
        if (ch == 16) lsa = value;
        else if (ch == 18) eal = value;
        else if (ch == 19) size = value;
        const ChanRec* e = next(1, ch, value, true);
        if (ch != 21 || !e) return;
        for (auto& [a, data] : e->outs) compare_out(a, static_cast<uint32_t>(data.size()), data.data());
        for (auto& [a, data] : e->lines) {
            std::memcpy(t.ls + a, data.data(), data.size());
            t.mark_ls(a, static_cast<uint32_t>(data.size()));
        }
    }
    uint32_t channel_count(uint32_t ch) override {
        const ChanRec* e = next(2, ch, 0, false);
        return e ? e->value : 0;
    }
    bool stop_and_signal(uint32_t code) override {
        next(3, code, 0, false);
        return false;
    }
    void halt(uint32_t pc, uint32_t word) override { next(4, pc, word, true); }
};

struct Exec {
    std::unique_ptr<spu::SpuThread> t;
    uint32_t index = 0;
    bool helper = false;
    bool have_job = false;
    Job cur;
    uint64_t t_start = 0;
    uint64_t cpu_start = 0;
    uint64_t insn_mark = 0;
    std::unique_ptr<Recorder> rec;
    std::unique_ptr<spu::SpuThread> shadow;
    bool verify_this = false;
    std::vector<uint8_t> ref_ls;
    spu::Context ref_ctx_start{};
    std::vector<uint8_t> start_ls;
};

std::atomic<uint64_t> g_job_seq{0};

void write_be32(uint8_t* p, uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(p, &v, 4);
}

void load_ls(spu::SpuThread& t, uint32_t lsa, const uint8_t* src, uint32_t size) {
    if (std::memcmp(t.ls + lsa, src, size) == 0) return;
    std::memcpy(t.ls + lsa, src, size);
    t.mark_ls(lsa, size);
}

void setup_job(spu::SpuThread& t, const Job& j, uint32_t index) {
    static const std::vector<uint8_t> pm = [] {
        std::vector<uint8_t> v(kPmSize);
        std::memcpy(v.data(), vm::ptr(kPmEa), kPmSize);
        return v;
    }();
    load_ls(t, kPmLs, pm.data(), kPmSize);
    load_ls(t, kCodeLs, vm::ptr(j.code_ea), j.code_size);
    uint8_t blk[0x100] = {};
    std::memcpy(blk, vm::ptr(j.cmd_ea), j.cmd_size);
    for (uint32_t off = 0; off + 8 <= j.param_off && off + 8 <= j.cmd_size; off += 8) {
        uint32_t hi;
        std::memcpy(&hi, blk + off, 4);
        hi = __builtin_bswap32(hi);
        if (((hi >> 9) & 7) != 5) std::memset(blk + off, 0, 8);
    }
    load_ls(t, kCmdLs, blk, sizeof blk);
    uint8_t api[0xc0] = {};
    const uint32_t tbl[] = {0x16f0, 0x2fd8, 0x1760, 0x17b8, 0x17c0, 0x17e0, 0x1af0, 0x1b10, 0x1a90};
    write_be32(api + 0x00, index);
    write_be32(api + 0x04, 0x1f);
    write_be32(api + 0x0c, j.spurs);
    write_be32(api + 0x10, j.wid << 16);
    write_be32(api + 0x14, 1);
    write_be32(api + 0x1c, 0x000201ff);
    for (int k = 0; k < 4; k++) write_be32(api + 0x20 + 4 * k, kCmdLs + j.param_off);
    for (int k = 0; k < 9; k++) write_be32(api + 0x30 + 4 * k, tbl[k]);
    write_be32(api + 0x60, j.list_hdr | 3);
    write_be32(api + 0x64, 0x1ff);
    write_be32(api + 0x6c, 1);
    write_be32(api + 0x80, 3);
    write_be32(api + 0x84, 0xffffffff);
    write_be32(api + 0x88, kPmEa);
    write_be32(api + 0x8c, kPmSize);
    for (int k = 0; k < 4; k++) write_be32(api + 0xa0 + 4 * k, 1);
    write_be32(api + 0xb0, 1);
    write_be32(api + 0xbc, 0x41);
    load_ls(t, kApiLs, api, sizeof api);
    uint8_t tags[0x20] = {};
    write_be32(tags, 0xff);
    load_ls(t, 0x1290, tags, sizeof tags);
    auto& r = t.ctx.gpr;
    for (int k = 0; k < 4; k++) {
        r[0].u32[k] = 0;
        r[3].u32[k] = kApiLs;
        r[4].u32[k] = 0;
        r[5].u32[k] = 0;
        r[6].u32[k] = 0;
    }
    uint32_t entry = kCodeLs + vm::read32(j.code_ea + 0x10);
    r[0].u32[3] = kJobRet;
    r[1].u32[3] = 0x3f990;
    r[5].u32[3] = entry;
    r[6].u32[3] = 0x1000;
    t.ctx.pc = entry & spu::PC_MASK;
    t.ctx.interrupts_enabled = true;
    t.ctx.halted = false;
    t.ctx.irq_check = false;
    t.tag_mask = t.tag_stat = t.stall_stat = t.atomic_stat = 0;
    t.tag_stat_count = t.stall_stat_count = t.atomic_stat_count = false;
    t.events = 0;
    t.event_mask = 0;
    t.event_count = false;
    t.raddr = 0;
}

void poison(spu::SpuThread& t, uint64_t seed, uint32_t mask) {
    uint64_t x = seed * 0x9e3779b97f4a7c15ull + 1;
    auto rnd = [&] {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return x;
    };
    for (uint32_t a = 0x17580; a < 0x40000; a += 8) {
        uint64_t v = rnd();
        bool stack = a >= 0x3f000;
        if ((stack && (mask & 4)) || (!stack && (mask & 2))) std::memcpy(t.ls + a, &v, 8);
    }
    static const uint32_t ranges[][2] = {{0x17580, 0x19300}, {0x19300, 0x1a000}, {0x1a000, 0x38a00}, {0x38a00, 0x391a0}, {0x391a0, 0x3b380}, {0x3b380, 0x3f000}};
    for (int k = 0; k < 6; k++) {
        if (!(mask & (0x100u << k))) continue;
        for (uint32_t a = ranges[k][0]; a < ranges[k][1]; a += 8) {
            uint64_t v = rnd();
            std::memcpy(t.ls + a, &v, 8);
        }
    }
    if (mask & 8) {
        for (uint32_t a = 0; a < 0xa00; a += 8) {
            uint64_t v = rnd();
            std::memcpy(t.ls + a, &v, 8);
        }
    }
    t.mark_all_ls();
    if (mask & 1) {
        for (int k = 7; k < 128; k++) {
            t.ctx.gpr[k].u64[0] = rnd();
            t.ctx.gpr[k].u64[1] = rnd();
        }
    }
}

bool trap(spu::SpuThread& t) {
    uint32_t pc = t.ctx.pc;
    if (pc == kLoadNextJob) {
        t.ctx.pc = t.ctx.gpr[0].u32[3] & spu::PC_MASK;
        return true;
    }
    if (pc == 0x16f0 || pc == 0x2fd8 || pc == 0x1760 || pc == 0x1af0 || pc == 0x1b10 || pc == 0x1a90) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 8) LOG_ERROR("ppujobs", "job called unsupported JobApi 0x%04x from 0x%05x", pc, t.ctx.gpr[0].u32[3]);
        t.ctx.pc = t.ctx.gpr[0].u32[3] & spu::PC_MASK;
        return true;
    }
    return false;
}

void finish(Exec& x);
bool fetch(Exec& x, bool block);
bool next_cb(spu::SpuThread& t);

bool shadow_next(spu::SpuThread& t) {
    (void)t;
    return false;
}

struct VerifyStats {
    std::atomic<uint64_t> runs[16]{};
    std::atomic<uint64_t> bad[16]{};
};
VerifyStats g_vstats;

void verify_job(Exec& x) {
    if (!x.shadow) {
        x.shadow = std::make_unique<spu::SpuThread>();
        x.shadow->id = 0x7f000000u | x.index;
        x.shadow->slot = 40 + x.index;
        x.shadow->name = "ppujob verify";
        x.shadow->job_mode = true;
        x.shadow->job_stop = kJobRet;
        x.shadow->job_next = shadow_next;
        x.shadow->job_trap = trap;
    }
    static const std::vector<uint32_t> masks = [] {
        std::vector<uint32_t> v = parse_hex_list(std::getenv("GOWA_PPU_JOBS_VPOISON"));
        if (v.empty()) v = {0, 1, 2, 4, 8, 15};
        return v;
    }();
    spu::SpuThread& s = *x.shadow;
    for (uint32_t mask : masks) {
        std::memcpy(s.ls, x.start_ls.data(), spu::LS_SIZE);
        s.mark_all_ls();
        std::memcpy(s.ctx.gpr, x.ref_ctx_start.gpr, sizeof s.ctx.gpr);
        poison(s, g_job_seq.load() + 77, mask);
        setup_job(s, x.cur, x.index);
        Replayer rp(s, x.rec->ev, *x.t);
        s.exec_host = &rp;
        s.stop_flag.store(false);
        s.exec();
        s.exec_host = nullptr;
        s.stop_flag.store(false);
        if (rp.error.empty() && rp.pos != x.rec->ev.size()) rp.error = "shadow consumed " + std::to_string(rp.pos) + " of " + std::to_string(x.rec->ev.size()) + " channel ops";
        uint32_t slotk = mask & 15;
        for (int k = 0; k < 6; k++) {
            if (mask & (0x100u << k)) slotk = 10 + k;
        }
        g_vstats.runs[slotk].fetch_add(1);
        g_stats.verified.fetch_add(1);
        static std::atomic<int> logged{0};
        if (!rp.error.empty()) {
            g_vstats.bad[slotk].fetch_add(1);
            g_stats.mismatches.fetch_add(1);
            if (logged.fetch_add(1) < 30) LOG_ERROR("ppujobs", "verify MISMATCH poison 0x%x job cmd 0x%08x code 0x%08x: %s", mask, x.cur.cmd_ea, x.cur.code_ea, rp.error.c_str());
        } else if (logged.load() < 60) {
            logged.fetch_add(1);
            LOG_INFO("ppujobs", "verify ok poison 0x%x job cmd 0x%08x: %zu channel ops, %u outputs, %u bytes", mask, x.cur.cmd_ea, x.rec->ev.size(), rp.puts, rp.put_bytes);
        }
    }
}

void finish(Exec& x) {
    if (!x.have_job) return;
    spu::SpuThread& t = *x.t;
    for (uint32_t i = 0; i < x.cur.ndeps; i++) decrement(x.cur.deps[i], x.cur.spurs);
    uint64_t dt = __rdtsc() - x.t_start;
    g_stats.cycles.fetch_add(dt, std::memory_order_relaxed);
    g_stats.insns.fetch_add(t.exec_insns - x.insn_mark, std::memory_order_relaxed);
    ULONG64 cyc = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cyc);
    g_stats.cpu.fetch_add(cyc - x.cpu_start, std::memory_order_relaxed);
    x.insn_mark = t.exec_insns;
    g_stats.jobs.fetch_add(1, std::memory_order_relaxed);
    if (x.helper) g_stats.helped.fetch_add(1, std::memory_order_relaxed);
    if (x.verify_this) {
        t.exec_host = nullptr;
        verify_job(x);
        x.verify_this = false;
    }
    x.insn_mark = t.exec_insns;
    x.cur.ls->pending.fetch_sub(1, std::memory_order_acq_rel);
    x.have_job = false;
}

bool fetch(Exec& x, bool block) {
    std::unique_lock lk(g_qm);
    if (block) g_qcv.wait(lk, [] { return !g_queue.empty(); });
    if (g_queue.empty()) return false;
    x.cur = g_queue.front();
    g_queue.pop_front();
    g_queued.fetch_sub(1, std::memory_order_relaxed);
    lk.unlock();
    x.have_job = true;
    x.t_start = __rdtsc();
    ULONG64 cyc = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cyc);
    x.cpu_start = cyc;
    uint64_t seq = g_job_seq.fetch_add(1);
    spu::SpuThread& t = *x.t;
    if (cfg().poison) poison(t, seq, cfg().poison);
    setup_job(t, x.cur, x.index);
    x.verify_this = cfg().verify && seq % cfg().verify == 0;
    if (x.verify_this) {
        if (!x.rec) x.rec = std::make_unique<Recorder>(t);
        x.rec->ev.clear();
        x.start_ls.assign(t.ls, t.ls + spu::LS_SIZE);
        x.ref_ctx_start = t.ctx;
        t.exec_host = x.rec.get();
    }
    return true;
}

bool next_cb(spu::SpuThread& t) {
    Exec& x = *static_cast<Exec*>(t.job_user);
    finish(x);
    return fetch(x, !x.helper);
}

std::unique_ptr<Exec> make_exec(uint32_t index, bool helper) {
    auto x = std::make_unique<Exec>();
    x->index = index;
    x->helper = helper;
    x->t = std::make_unique<spu::SpuThread>();
    spu::SpuThread& t = *x->t;
    t.id = 0x7e000000u | index;
    t.slot = 32 + index;
    t.name = helper ? "ppujob helper" : "ppujob worker";
    t.job_mode = true;
    t.job_stop = kJobRet;
    t.job_next = next_cb;
    t.job_trap = trap;
    t.job_user = x.get();
    t.ctx.pc = kJobRet;
    return x;
}

void worker_main(int k) {
    wchar_t w[64];
    swprintf(w, 64, L"ppujob worker %d", k);
    SetThreadDescription(GetCurrentThread(), w);
    if (k < static_cast<int>(cfg().cpus.size()) && cfg().cpus[k] >= 0 && cfg().cpus[k] < 64) SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << cfg().cpus[k]);
    _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x6000u | 0x8040u);
    spu::hle::install_builtin();
    if (k == 0 && std::getenv("GOWA_PROFILE_PPUJOB")) {
        HANDLE h = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
        crash::profile_thread(h, "ppujob 0");
    }
    auto x = make_exec(static_cast<uint32_t>(k), false);
    x->t->ctx.pc = kJobRet;
    x->t->exec();
}

void stats_thread() {
    uint64_t lj = 0, lh = 0, lc = 0, lw = 0, lwc = 0, lcap = 0, lfb = 0, lin = 0, lcp = 0;
    double hz = 0;
    {
        auto c0 = std::chrono::steady_clock::now();
        uint64_t t0 = __rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        hz = double(__rdtsc() - t0) / std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
    }
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        uint64_t j = g_stats.jobs.load(), h = g_stats.helped.load(), c = g_stats.cycles.load(), wn = g_stats.waits.load(), wc = g_stats.wait_cycles.load();
        uint64_t cap = g_stats.captured.load(), fb = g_stats.fallback.load();
        uint64_t dj = j - lj;
        uint64_t in = g_stats.insns.load();
        uint64_t cp = g_stats.cpu.load();
        LOG_INFO("ppujobs", "5s: %.0f emulated kinsn/job, thread cpu %.1f us/job", dj ? double(in - lin) / 1e3 / double(dj) : 0.0, dj ? double(cp - lcp) / hz * 1e6 / double(dj) : 0.0);
        lin = in;
        lcp = cp;
        LOG_INFO("ppujobs", "5s: %llu jobs (%llu by waiter), %.1f us/job, host %.1f ms/s, %llu waits %.1f ms/s, captured %llu fallback %llu, verified %llu mismatches %llu", static_cast<unsigned long long>(dj),
                 static_cast<unsigned long long>(h - lh), dj ? double(c - lc) / hz * 1e6 / double(dj) : 0.0, double(c - lc) / hz * 1e3 / 5.0, static_cast<unsigned long long>(wn - lw), double(wc - lwc) / hz * 1e3 / 5.0,
                 static_cast<unsigned long long>(cap - lcap), static_cast<unsigned long long>(fb - lfb), static_cast<unsigned long long>(g_stats.verified.load()), static_cast<unsigned long long>(g_stats.mismatches.load()));
        lj = j;
        lh = h;
        lc = c;
        lw = wn;
        lwc = wc;
        lcap = cap;
        lfb = fb;
        if (cfg().verify) {
            std::string v;
            for (int k = 0; k < 16; k++) {
                if (!g_vstats.runs[k].load()) continue;
                char b[48];
                std::snprintf(b, sizeof b, " p%x %llu/%llu", k, static_cast<unsigned long long>(g_vstats.bad[k].load()), static_cast<unsigned long long>(g_vstats.runs[k].load()));
                v += b;
            }
            LOG_INFO("ppujobs", "verify bad/runs by poison mask:%s", v.c_str());
        }
    }
}

void start_once() {
    static std::once_flag once;
    std::call_once(once, [] {
        LOG_INFO("ppujobs", "PPU-level job execution on: %d workers, waiter helps %d, verify every %u, modules %zu", cfg().threads, cfg().help ? 1 : 0, cfg().verify, cfg().mods.size());
        for (int k = 0; k < cfg().threads; k++) std::thread(worker_main, k).detach();
        std::thread(stats_thread).detach();
    });
}

struct Capture {
    bool active = false;
    bool real_push = false;
    std::vector<Job> jobs;
};
thread_local Capture t_cap;

}

bool enabled() { return cfg().on; }

bool on_submit(ppu::Context& ctx) {
    if (!cfg().on) return false;
    uint32_t mod = static_cast<uint32_t>(ctx.gpr[5]);
    if (std::find(cfg().mods.begin(), cfg().mods.end(), mod) == cfg().mods.end()) return false;
    start_once();
    t_cap.active = true;
    t_cap.real_push = false;
    t_cap.jobs.clear();
    return false;
}

bool on_push(ppu::Context& ctx) {
    if (!t_cap.active) return false;
    Job j;
    j.header = ctx.gpr[5];
    j.cmd_ea = static_cast<uint32_t>(j.header);
    j.cmd_size = static_cast<uint32_t>(j.header >> 48);
    j.list_obj = static_cast<uint32_t>(ctx.gpr[4]);
    bool ok = ((j.header >> 32) & 0xffff) == 1 && vm::range_mapped(j.list_obj, 0x40);
    if (ok) {
        j.list_hdr = vm::read32(j.list_obj);
        j.spurs = vm::read32(j.list_obj + 4);
        j.wid = vm::read32(j.list_obj + 8);
        ok = j.spurs && vm::range_mapped(j.spurs, 0x80) && j.wid < 32 && parse(j);
    }
    if (!ok) {
        t_cap.real_push = true;
        g_stats.fallback.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 8) LOG_WARN("ppujobs", "job header 0x%016llx not supported, left on the WWS list", static_cast<unsigned long long>(j.header));
        return false;
    }
    t_cap.jobs.push_back(j);
    return true;
}

bool on_kick(ppu::Context& ctx) {
    if (!t_cap.active) return false;
    t_cap.active = false;
    if (t_cap.jobs.empty()) return false;
    uint32_t obj = static_cast<uint32_t>(ctx.gpr[3]);
    ListState* ls = list_state(obj, true);
    if (!ls) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 4) LOG_ERROR("ppujobs", "too many job lists, running %zu jobs inline", t_cap.jobs.size());
    }
    uint64_t now = __rdtsc();
    for (Job& j : t_cap.jobs) {
        j.ls = ls;
        j.t_submit = now;
    }
    if (!ls) return false;
    ls->pending.fetch_add(static_cast<int32_t>(t_cap.jobs.size()), std::memory_order_acq_rel);
    {
        std::lock_guard lk(g_qm);
        for (Job& j : t_cap.jobs) g_queue.push_back(j);
        g_queued.fetch_add(static_cast<uint32_t>(t_cap.jobs.size()), std::memory_order_relaxed);
    }
    g_qcv.notify_all();
    g_stats.captured.fetch_add(t_cap.jobs.size(), std::memory_order_relaxed);
    t_cap.jobs.clear();
    return !t_cap.real_push;
}

bool on_wait(ppu::Context& ctx) {
    if (!cfg().on) return false;
    ListState* ls = list_state(static_cast<uint32_t>(ctx.gpr[3]), false);
    if (!ls || ls->pending.load(std::memory_order_acquire) <= 0) return false;
    uint64_t t0 = __rdtsc();
    if (cfg().help) {
        static thread_local std::unique_ptr<Exec> helper;
        if (!helper) {
            spu::hle::install_builtin();
            helper = make_exec(31, true);
        }
        unsigned csr = _mm_getcsr();
        _mm_setcsr((csr & ~0x6000u) | 0x6000u | 0x8040u);
        while (ls->pending.load(std::memory_order_acquire) > 0 && g_queued.load(std::memory_order_relaxed)) {
            helper->t->ctx.pc = kJobRet;
            helper->t->exec();
        }
        _mm_setcsr(csr);
    }
    for (uint32_t i = 0; ls->pending.load(std::memory_order_acquire) > 0; i++) {
        if (i < 64) _mm_pause();
        else if (i < 4096) SwitchToThread();
        else Sleep(0);
    }
    uint64_t t1 = __rdtsc();
    g_stats.waits.fetch_add(1, std::memory_order_relaxed);
    g_stats.wait_cycles.fetch_add(t1 - t0, std::memory_order_relaxed);
    if (critpath::g_on) critpath::emit_wait(critpath::K_WWS_WAIT, t0, t1, static_cast<uint32_t>(ctx.gpr[3]), 3, 0x705680);
    return false;
}

}
