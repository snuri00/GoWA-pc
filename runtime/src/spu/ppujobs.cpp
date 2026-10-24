#include "runtime/ppujobs.h"

#include "hle.h"
#include "thread.h"
#include "renjob/renjob_adapters.h"
#include "renjob/renjob_whole.h"

#include "ppu/context.h"
#include "runtime/critpath.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/reservation.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <unordered_map>
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
    bool native = false;
    bool spu_host = false;
    bool ab = false;
    int worker_prio = 0;
    bool worker_prio_set = false;
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
        if (const char* n = std::getenv("GOWA_PPU_JOBS_NATIVE")) {
            c.native = n[0] && n[0] != '0';
            c.spu_host = n[0] == 's';
            c.ab = n[0] == 'a';
        }
        if (const char* p = std::getenv("GOWA_PPU_JOBS_PRIO")) {
            c.worker_prio = std::atoi(p);
            c.worker_prio_set = true;
        }
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
    std::atomic<uint64_t> native_done{0}, native_resumed{0}, native_cycles{0}, state_diff{0}, rmw_puts{0}, slow_dma{0};
    std::atomic<uint64_t> n_jobs{0}, n_tsc{0}, n_cpu{0}, e_jobs{0}, e_tsc{0}, e_cpu{0};
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
    std::vector<uint32_t> line_ea;
    std::vector<uint32_t> out_ea;
    bool ok = true;
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
            uint32_t cur_ea = eal;
            auto grab = [&](uint32_t a, uint32_t n) {
                a &= 0x3ffff;
                if (n > 0x40000 - a) n = 0x40000 - a;
                r.lines.push_back({a, std::vector<uint8_t>(t.ls + a, t.ls + a + n)});
                r.line_ea.push_back(cur_ea);
            };
            auto out = [&](uint32_t a, uint32_t n) {
                a &= 0x3ffff;
                if (n > 0x40000 - a) n = 0x40000 - a;
                r.outs.push_back({a, std::vector<uint8_t>(t.ls + a, t.ls + a + n)});
                r.out_ea.push_back(cur_ea);
            };
            if (cmd == 0xb4) r.ok = t.atomic_stat == 0;
            bool put = (cmd >= 0x20 && cmd <= 0x26) || (cmd >= 0x30 && cmd <= 0x36) || cmd == 0xa0 || cmd == 0xa1 || cmd == 0xa2;
            if (put && ((cmd & 0xf) >= 4 && (cmd & 0xf) <= 6)) {
                uint32_t cur = lsa & 0x3fff0, list = eal & 0x3fff8;
                for (uint32_t n = size; n >= 8; n -= 8) {
                    uint8_t* e = t.ls + list;
                    uint32_t ts = ((uint32_t(e[2]) << 8) | e[3]) & 0x7fff;
                    uint32_t ea;
                    std::memcpy(&ea, e + 4, 4);
                    ea = __builtin_bswap32(ea);
                    cur_ea = ea;
                    out(cur | (ea & 15), ts);
                    cur = (cur + ((ts + 15) & ~15u)) & 0x3fff0;
                    list = (list + 8) & 0x3fff8;
                }
            } else if (put) {
                out(lsa, cmd >= 0xa0 ? 4 : size);
            } else if (cmd == 0xb4 || cmd == 0xb0 || cmd == 0xb8) {
                cur_ea = eal & ~127u;
                out(lsa & ~127u, 128);
            }
            if (cmd == 0x40 || cmd == 0x41 || cmd == 0x42) grab(lsa, size);
            else if (cmd == 0xd0) {
                cur_ea = eal & ~127u;
                grab(lsa & ~127u, 128);
            }
            else if (cmd == 0x44 || cmd == 0x45 || cmd == 0x46) {
                uint32_t cur = lsa & 0x3fff0, list = eal & 0x3fff8;
                for (uint32_t n = size; n >= 8; n -= 8) {
                    uint8_t* e = t.ls + list;
                    uint32_t ts = ((uint32_t(e[2]) << 8) | e[3]) & 0x7fff;
                    uint32_t ea;
                    std::memcpy(&ea, e + 4, 4);
                    ea = __builtin_bswap32(ea);
                    cur_ea = ea;
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

struct DmaStats {
    std::atomic<uint64_t> wr{0}, rd{0}, get{0}, put{0}, bget{0}, bput{0};
};
DmaStats g_dstats;

struct FastHost final : renjob::ChanHost {
    spu::SpuThread& t;
    uint32_t table = 0;
    uint32_t get_lo[8] = {}, get_hi[8] = {};
    uint32_t ngets = 0;
    uint32_t rmw = 0, slow = 0;
    uint32_t n_wr = 0, n_rd = 0, n_get = 0, n_put = 0, b_get = 0, b_put = 0, n_atom = 0;
    explicit FastHost(spu::SpuThread& th) : t(th) {
        table = vm::is_mapped(0x9B2298) ? vm::read32(0x9B2298) : 0;
    }
    bool in_table(uint32_t ea, uint32_t size) const { return table && ea < table + 0xA280 && ea + size > table; }
    void note_get(uint32_t ea, uint32_t size) {
        get_lo[ngets & 7] = ea;
        get_hi[ngets & 7] = ea + size;
        ngets++;
    }
    void note_put(uint32_t ea, uint32_t size) {
        uint32_t n = ngets < 8 ? ngets : 8;
        for (uint32_t i = 0; i < n; i++) {
            if (ea < get_hi[i] && ea + size > get_lo[i]) {
                rmw++;
                return;
            }
        }
    }
    uint32_t rd(uint32_t ch) override {
        n_rd++;
        switch (ch) {
        case 13: return t.ctx.interrupts_enabled ? 1 : 0;
        case 24:
            if (t.tag_stat_count) {
                t.tag_stat_count = false;
                return t.tag_stat;
            }
            break;
        case 27:
            if (t.atomic_stat_count) {
                t.atomic_stat_count = false;
                return t.atomic_stat;
            }
            break;
        default: break;
        }
        return t.read_channel(ch);
    }
    uint32_t cnt(uint32_t ch) override {
        switch (ch) {
        case 21: return 16;
        case 23: return 1;
        case 24: return t.tag_stat_count;
        case 27: return t.atomic_stat_count;
        default: return t.channel_count(ch);
        }
    }
    void wr(uint32_t ch, uint32_t v) override {
        n_wr++;
        switch (ch) {
        case 16: t.mfc_lsa = v; return;
        case 17: return;
        case 18: t.mfc_eal = v; return;
        case 19: t.mfc_size = v > 0xFFFF ? 0xFFFF : v; return;
        case 20: t.mfc_tag = v & 0x1F; return;
        case 21: mfc(v & 0xFF); return;
        case 22: t.tag_mask = v; return;
        case 23:
            t.tag_stat = t.tag_mask;
            if (v == 1 && t.tag_mask == 0) return;
            t.tag_stat_count = true;
            return;
        default: t.write_channel(ch, v); return;
        }
    }
    bool plain(uint32_t ea, uint32_t size, uint32_t lsa) const {
        return size && size <= 0x4000 && ea >= 0x10000 && ea < 0xF0000000u && (lsa & 0x3FFFF) + size <= spu::LS_SIZE && vm::is_mapped(ea) && vm::is_mapped(ea + size - 1);
    }
    void get(uint32_t lsa, uint32_t ea, uint32_t size) {
        n_get++;
        b_get += size;
        lsa &= 0x3FFFF;
        if (vm::is_immutable(ea, size)) {
            std::memcpy(t.ls + lsa, vm::ptr(ea), size);
        } else {
            uint32_t lo = 0xFFFFFFFFu, hi = 0;
            rsv::copy_range_if_changed(ea, t.ls + lsa, size, lo, hi);
        }
        if (lsa < 0x17580) t.mark_ls(lsa, size);
        note_get(ea, size);
    }
    void put(uint32_t lsa, uint32_t ea, uint32_t size) {
        n_put++;
        b_put += size;
        lsa &= 0x3FFFF;
        note_put(ea, size);
        rsv::store_range(ea, t.ls + lsa, size);
    }
    void slow_path(uint32_t cmd) {
        slow++;
        t.write_channel(21, cmd);
    }
    void mfc(uint32_t cmd) {
        uint32_t lsa = t.mfc_lsa, ea = t.mfc_eal, size = t.mfc_size;
        switch (cmd) {
        case 0x40: case 0x41: case 0x42:
            if (!plain(ea, size, lsa)) return slow_path(cmd);
            get(lsa, ea, size);
            return;
        case 0x20: case 0x21: case 0x22:
            if (!plain(ea, size, lsa) || in_table(ea, size)) return slow_path(cmd);
            put(lsa, ea, size);
            return;
        case 0x44: case 0x45: case 0x46:
        case 0x24: case 0x25: case 0x26: {
            bool is_put = cmd < 0x40;
            uint32_t list = ea & 0x3FFF8;
            uint32_t cur = lsa & 0x3FFF0;
            for (uint32_t n = size, l = list, c = cur; n >= 8; n -= 8) {
                const uint8_t* e = t.ls + l;
                uint32_t ts = ((uint32_t(e[2]) << 8) | e[3]) & 0x7FFF;
                uint32_t a;
                std::memcpy(&a, e + 4, 4);
                a = __builtin_bswap32(a);
                if ((e[0] & 0x80) || (ts && (!plain(a, ts, c | (a & 15)) || (is_put && in_table(a, ts))))) return slow_path(cmd);
                c = (c + ((ts + 15) & ~15u)) & 0x3FFF0;
                l = (l + 8) & 0x3FFF8;
                if (l + 8 > spu::LS_SIZE && n > 8) return slow_path(cmd);
            }
            for (uint32_t n = size; n >= 8; n -= 8) {
                const uint8_t* e = t.ls + list;
                uint32_t ts = ((uint32_t(e[2]) << 8) | e[3]) & 0x7FFF;
                uint32_t a;
                std::memcpy(&a, e + 4, 4);
                a = __builtin_bswap32(a);
                if (ts) {
                    if (is_put) put(cur | (a & 15), a, ts);
                    else get(cur | (a & 15), a, ts);
                }
                cur = (cur + ((ts + 15) & ~15u)) & 0x3FFF0;
                list = (list + 8) & 0x3FFF8;
            }
            return;
        }
        case 0xD0: {
            uint32_t line = ea & ~127u;
            if (ea < 0x10000 || !vm::is_mapped(line)) return slow_path(cmd);
            uint64_t ver = rsv::snapshot(line, t.rdata);
            t.raddr = line;
            t.rtime = ver;
            std::memcpy(t.ls + (lsa & 0x3FF80), t.rdata, 128);
            t.atomic_stat = 4;
            t.atomic_stat_count = true;
            return;
        }
        case 0xB4: {
            uint32_t line = ea & ~127u;
            if (ea < 0x10000 || !vm::is_mapped(line)) return slow_path(cmd);
            bool ok = t.raddr == line && rsv::commit_if(line, t.rtime, t.rdata, t.ls + (lsa & 0x3FF80), 0, 128);
            t.raddr = 0;
            t.atomic_stat = ok ? 0 : 1;
            t.atomic_stat_count = true;
            return;
        }
        case 0xB0: case 0xB8: {
            uint32_t line = ea & ~127u;
            if (ea < 0x10000 || !vm::is_mapped(line)) return slow_path(cmd);
            rsv::store_unconditional(line, t.ls + (lsa & 0x3FF80));
            if (cmd == 0xB0) {
                t.atomic_stat = 2;
                t.atomic_stat_count = true;
            }
            return;
        }
        case 0xC0: case 0xC8: case 0xCC: case 0x80: case 0x81:
            std::atomic_thread_fence(std::memory_order_seq_cst);
            return;
        default:
            return slow_path(cmd);
        }
    }
};

struct SpuHostChan final : renjob::ChanHost {
    spu::Host& h;
    spu::SpuThread& t;
    SpuHostChan(spu::Host& host, spu::SpuThread& th) : h(host), t(th) {}
    uint32_t rd(uint32_t ch) override {
        uint32_t v = h.read_channel(ch);
        abort = t.ctx.halted;
        return v;
    }
    void wr(uint32_t ch, uint32_t v) override {
        h.write_channel(ch, v);
        abort = t.ctx.halted;
    }
    uint32_t cnt(uint32_t ch) override { return h.channel_count(ch); }
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
    if (x.verify_this && !cfg().native) {
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
        if (!cfg().native) t.exec_host = x.rec.get();
    }
    return true;
}

bool next_cb(spu::SpuThread& t) {
    Exec& x = *static_cast<Exec*>(t.job_user);
    finish(x);
    return fetch(x, !x.helper);
}

bool single_next(spu::SpuThread&) { return false; }

void emulate_rest(spu::SpuThread& t, spu::Host* host) {
    for (uint32_t k = 0; k < 64; k++) t.clear_snap(k);
    t.mark_all_ls();
    t.exec_host = host;
    t.stop_flag.store(false);
    t.exec();
    t.exec_host = nullptr;
}

bool whole_ok(const spu::SpuThread& t, const Job& j) {
    return renjob::image_loaded(t.ls) && kCodeLs + vm::read32(j.code_ea + 0x10) == renjob::kWholeEntry;
}

struct FailLog {
    std::mutex m;
    std::vector<std::pair<uint32_t, uint32_t>> seen;
    void note(uint32_t pc, uint32_t miss) {
        std::lock_guard lk(m);
        for (auto& [p, n] : seen) {
            if (p == pc) {
                n++;
                return;
            }
        }
        seen.push_back({pc, 1});
        LOG_WARN("ppujobs", "native RenJob resumed in the emulator at 0x%05x (icall target 0x%05x)", pc, miss);
    }
};
FailLog g_faillog;

std::string diff_state(const spu::SpuThread& a, const spu::SpuThread& b) {
    std::string out;
    for (int r = 0; r < 128 && out.size() < 300; r++) {
        if (std::memcmp(&a.ctx.gpr[r], &b.ctx.gpr[r], 16) == 0) continue;
        char t[96];
        std::snprintf(t, sizeof t, " r%d %08x%08x%08x%08x/%08x%08x%08x%08x", r, a.ctx.gpr[r].u32[3], a.ctx.gpr[r].u32[2], a.ctx.gpr[r].u32[1], a.ctx.gpr[r].u32[0], b.ctx.gpr[r].u32[3],
                      b.ctx.gpr[r].u32[2], b.ctx.gpr[r].u32[1], b.ctx.gpr[r].u32[0]);
        out += t;
    }
    uint32_t lines = 0, first = 0;
    for (uint32_t q = 0; q < spu::LS_SIZE; q += 128) {
        if (std::memcmp(a.ls + q, b.ls + q, 128) == 0) continue;
        if (!lines) first = q;
        lines++;
    }
    if (lines) {
        char t[80];
        std::snprintf(t, sizeof t, " ls lines %u first 0x%05x", lines, first);
        out += t;
    }
    if (a.ctx.pc != b.ctx.pc) out += " pc " + std::to_string(a.ctx.pc) + "/" + std::to_string(b.ctx.pc);
    return out;
}

void verify_native(Exec& x) {
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
    spu::SpuThread& s = *x.shadow;
    std::memcpy(s.ls, x.start_ls.data(), spu::LS_SIZE);
    s.mark_all_ls();
    std::memcpy(s.ctx.gpr, x.ref_ctx_start.gpr, sizeof s.ctx.gpr);
    setup_job(s, x.cur, x.index);
    Replayer rp(s, x.rec->ev, *x.t);
    SpuHostChan ch(rp, s);
    s.stop_flag.store(false);
    renjob::WholeResult r;
    bool can = whole_ok(s, x.cur);
    if (can) r = renjob::run_whole(s.ls, s.ctx, ch);
    if (r.done) s.ctx.pc = kJobRet;
    bool resumed = false;
    if (!r.done && !s.ctx.halted && rp.error.empty()) {
        if (can) s.ctx.pc = r.fail_pc;
        emulate_rest(s, &rp);
        resumed = true;
    }
    s.stop_flag.store(false);
    if (rp.error.empty() && rp.pos != x.rec->ev.size()) rp.error = "native consumed " + std::to_string(rp.pos) + " of " + std::to_string(x.rec->ev.size()) + " channel ops";
    g_vstats.runs[0].fetch_add(1);
    g_stats.verified.fetch_add(1);
    std::string sd = diff_state(s, *x.t);
    if (!sd.empty()) g_stats.state_diff.fetch_add(1);
    static std::atomic<int> logged{0};
    if (!rp.error.empty()) {
        g_vstats.bad[0].fetch_add(1);
        g_stats.mismatches.fetch_add(1);
        if (logged.fetch_add(1) < 30) LOG_ERROR("ppujobs", "native verify MISMATCH job cmd 0x%08x (resumed %d at 0x%05x): %s;%s", x.cur.cmd_ea, resumed ? 1 : 0, r.fail_pc, rp.error.c_str(), sd.c_str());
    } else if (logged.load() < 40) {
        logged.fetch_add(1);
        LOG_INFO("ppujobs", "native verify ok job cmd 0x%08x: %zu channel ops, %u outputs, %u bytes, resumed %d, state%s", x.cur.cmd_ea, x.rec->ev.size(), rp.puts, rp.put_bytes, resumed ? 1 : 0, sd.empty() ? " identical" : sd.c_str());
    }
}

uint64_t thread_cycles() {
    ULONG64 c = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c);
    return c;
}

void run_native_body(Exec& x, bool emu_only);

struct SandboxHost final : renjob::ChanHost {
    spu::SpuThread& t;
    const std::unordered_map<uint32_t, std::vector<uint8_t>>& inputs;
    std::unordered_map<uint32_t, uint32_t> rpos;
    std::vector<uint8_t> llc_ok;
    size_t llc_pos = 0;
    std::map<uint32_t, uint8_t> writes;
    uint32_t live_bytes = 0;
    alignas(16) uint8_t rline[128] = {};
    SandboxHost(spu::SpuThread& th, const std::unordered_map<uint32_t, std::vector<uint8_t>>& in) : t(th), inputs(in) {}
    uint8_t byte(uint32_t ea) {
        auto i = inputs.find(ea);
        if (i != inputs.end()) {
            uint32_t k = rpos[ea]++;
            return i->second[k < i->second.size() ? k : i->second.size() - 1];
        }
        auto w = writes.find(ea);
        if (w != writes.end()) return w->second;
        live_bytes++;
        return vm::is_mapped(ea) ? vm::ptr(ea)[0] : 0;
    }
    void load(uint32_t lsa, uint32_t ea, uint32_t n) {
        for (uint32_t k = 0; k < n; k++) t.ls[(lsa + k) & 0x3FFFF] = byte(ea + k);
    }
    void store(uint32_t lsa, uint32_t ea, uint32_t n) {
        for (uint32_t k = 0; k < n; k++) writes[ea + k] = t.ls[(lsa + k) & 0x3FFFF];
    }
    uint32_t rd(uint32_t ch) override {
        switch (ch) {
        case 13: return t.ctx.interrupts_enabled ? 1 : 0;
        case 24:
            t.tag_stat_count = false;
            return t.tag_stat;
        case 27:
            t.atomic_stat_count = false;
            return t.atomic_stat;
        case 8: return 0;
        default: abort = true; return 0;
        }
    }
    uint32_t cnt(uint32_t ch) override {
        switch (ch) {
        case 21: return 16;
        case 23: return 1;
        case 24: return t.tag_stat_count;
        case 27: return t.atomic_stat_count;
        default: abort = true; return 0;
        }
    }
    void wr(uint32_t ch, uint32_t v) override {
        switch (ch) {
        case 16: t.mfc_lsa = v; return;
        case 17: return;
        case 18: t.mfc_eal = v; return;
        case 19: t.mfc_size = v > 0xFFFF ? 0xFFFF : v; return;
        case 20: t.mfc_tag = v & 0x1F; return;
        case 22: t.tag_mask = v; return;
        case 23:
            t.tag_stat = t.tag_mask;
            if (v == 1 && t.tag_mask == 0) return;
            t.tag_stat_count = true;
            return;
        case 21: break;
        default: abort = true; return;
        }
        uint32_t cmd = v & 0xFF, lsa = t.mfc_lsa & 0x3FFFF, ea = t.mfc_eal, size = t.mfc_size;
        switch (cmd) {
        case 0x40: case 0x41: case 0x42: load(lsa, ea, size); return;
        case 0x20: case 0x21: case 0x22: store(lsa, ea, size); return;
        case 0x44: case 0x45: case 0x46: case 0x24: case 0x25: case 0x26: {
            uint32_t cur = lsa & 0x3FFF0, list = ea & 0x3FFF8;
            for (uint32_t n = size; n >= 8; n -= 8) {
                const uint8_t* e = t.ls + list;
                uint32_t ts = ((uint32_t(e[2]) << 8) | e[3]) & 0x7FFF;
                uint32_t a;
                std::memcpy(&a, e + 4, 4);
                a = __builtin_bswap32(a);
                if (cmd < 0x40) store(cur | (a & 15), a, ts);
                else load(cur | (a & 15), a, ts);
                cur = (cur + ((ts + 15) & ~15u)) & 0x3FFF0;
                list = (list + 8) & 0x3FFF8;
            }
            return;
        }
        case 0xD0:
            load(lsa & 0x3FF80, ea & ~127u, 128);
            std::memcpy(rline, t.ls + (lsa & 0x3FF80), 128);
            t.atomic_stat = 4;
            t.atomic_stat_count = true;
            return;
        case 0xB4: {
            bool ok = llc_pos < llc_ok.size() ? llc_ok[llc_pos] != 0 : true;
            llc_pos++;
            if (ok) store(lsa & 0x3FF80, ea & ~127u, 128);
            t.atomic_stat = ok ? 0 : 1;
            t.atomic_stat_count = true;
            return;
        }
        case 0xB0: case 0xB8:
            store(lsa & 0x3FF80, ea & ~127u, 128);
            if (cmd == 0xB0) {
                t.atomic_stat = 2;
                t.atomic_stat_count = true;
            }
            return;
        case 0xC0: case 0xC8: case 0xCC: case 0x80: case 0x81: return;
        default: abort = true; return;
        }
    }
};

struct SandboxStats {
    std::atomic<uint64_t> runs{0}, same{0}, differ{0}, skipped{0}, seq_differs{0};
};
SandboxStats g_sbx;
std::mutex g_sbx_m;
std::map<uint32_t, std::array<uint64_t, 3>> g_sbx_mask;

void sbx_note(uint32_t mask, int k) {
    std::lock_guard lk(g_sbx_m);
    g_sbx_mask[mask][k]++;
}

void verify_sandbox(Exec& x, uint32_t mask) {
    if (!x.shadow) return;
    std::unordered_map<uint32_t, std::vector<uint8_t>> inputs;
    std::map<uint32_t, uint8_t> ref;
    for (const ChanRec& e : x.rec->ev) {
        for (size_t i = 0; i < e.lines.size() && i < e.line_ea.size(); i++) {
            const auto& d = e.lines[i].second;
            for (uint32_t k = 0; k < d.size(); k++) inputs[e.line_ea[i] + k].push_back(d[k]);
        }
        if (!e.ok) continue;
        for (size_t i = 0; i < e.outs.size() && i < e.out_ea.size(); i++) {
            const auto& d = e.outs[i].second;
            for (uint32_t k = 0; k < d.size(); k++) ref[e.out_ea[i] + k] = d[k];
        }
    }
    spu::SpuThread& s = *x.shadow;
    std::memcpy(s.ls, x.start_ls.data(), spu::LS_SIZE);
    std::memcpy(s.ctx.gpr, x.ref_ctx_start.gpr, sizeof s.ctx.gpr);
    static thread_local std::vector<uint8_t> prev_cache;
    if (mask & 0x10000) {
        bool have = prev_cache.size() == 0x38a00 - 0x19300;
        if (have) std::memcpy(s.ls + 0x19300, prev_cache.data(), prev_cache.size());
        prev_cache.assign(x.start_ls.begin() + 0x19300, x.start_ls.begin() + 0x38a00);
        if (!have) return;
    }
    poison(s, g_job_seq.load() * 31 + 5, mask & 0xffff);
    setup_job(s, x.cur, x.index);
    SandboxHost h(s, inputs);
    for (const ChanRec& e : x.rec->ev)
        if (e.kind == 1 && e.ch == 21 && (e.value & 0xff) == 0xb4) h.llc_ok.push_back(e.ok ? 1 : 0);
    renjob::WholeResult r = renjob::run_whole(s.ls, s.ctx, h);
    g_sbx.runs.fetch_add(1);
    static std::atomic<int> logged{0};
    if (!r.done || h.abort) {
        g_sbx.skipped.fetch_add(1);
        sbx_note(mask, 2);
        if (logged.fetch_add(1) < 10) LOG_WARN("ppujobs", "sandbox verify skipped: done %d abort %d fail pc 0x%05x", r.done ? 1 : 0, h.abort ? 1 : 0, r.fail_pc);
        return;
    }
    uint32_t diff = 0, first = 0;
    for (auto& [a, v] : ref) {
        auto it = h.writes.find(a);
        if (it == h.writes.end() || it->second != v) {
            if (!diff) first = a;
            diff++;
        }
    }
    uint32_t extra = 0;
    for (auto& [a, v] : h.writes) {
        if (!ref.count(a)) {
            if (!diff && !extra) first = a;
            extra++;
        }
    }
    static std::atomic<int> detail_logged{0};
    static const uint32_t detail_mask = std::getenv("GOWA_PPU_JOBS_SBXDETAIL") ? static_cast<uint32_t>(std::strtoul(std::getenv("GOWA_PPU_JOBS_SBXDETAIL"), nullptr, 16)) : 0x10000u;
    if ((diff || extra) && mask == detail_mask && detail_logged.fetch_add(1) < 12) {
        std::string d;
        uint32_t shown = 0, prev = 0;
        for (auto& [a, v] : ref) {
            auto it = h.writes.find(a);
            bool bad = it == h.writes.end() || it->second != v;
            if (!bad || shown >= 64) continue;
            char b[48];
            std::snprintf(b, sizeof b, "%s%08x:%02x/%s", a == prev + 1 ? "," : " ", a, v, it == h.writes.end() ? "--" : (std::to_string(it->second)).c_str());
            d += b;
            prev = a;
            shown++;
        }
        d += " | extra";
        shown = 0;
        for (auto& [a, v] : h.writes) {
            if (ref.count(a) || shown >= 24) continue;
            char b[32];
            std::snprintf(b, sizeof b, " %08x:%02x", a, v);
            d += b;
            shown++;
        }
        LOG_INFO("ppujobs", "sandbox diff detail mask 0x%x job cmd 0x%08x live %u:%s", mask, x.cur.cmd_ea, h.live_bytes, d.c_str());
    }
    if (diff || extra) {
        g_sbx.differ.fetch_add(1);
        sbx_note(mask, 1);
        if (logged.fetch_add(1) < 30) LOG_ERROR("ppujobs", "sandbox verify DIFFERS poison 0x%x job cmd 0x%08x: %u ref bytes differ, %u extra bytes, first ea 0x%08x, live input bytes %u", mask, x.cur.cmd_ea, diff, extra, first, h.live_bytes);
    } else {
        g_sbx.same.fetch_add(1);
        sbx_note(mask, 0);
        if (logged.fetch_add(1) < 20) LOG_INFO("ppujobs", "sandbox verify same poison 0x%x job cmd 0x%08x: %zu written bytes, live input bytes %u", mask, x.cur.cmd_ea, ref.size(), h.live_bytes);
    }
}

void run_native_job(Exec& x) {
    spu::SpuThread& t = *x.t;
    if (x.verify_this) {
        emulate_rest(t, x.rec.get());
        verify_native(x);
        static const std::vector<uint32_t> smasks = parse_hex_list(std::getenv("GOWA_PPU_JOBS_SANDBOX"));
        for (uint32_t m : smasks) verify_sandbox(x, m);
        x.verify_this = false;
        return;
    }
    static std::atomic<uint64_t> seq{0};
    bool emu = cfg().ab && (seq.fetch_add(1, std::memory_order_relaxed) & 1);
    uint64_t t0 = __rdtsc(), c0 = thread_cycles();
    run_native_body(x, emu);
    uint64_t t1 = __rdtsc(), c1 = thread_cycles();
    if (emu) {
        g_stats.e_jobs.fetch_add(1, std::memory_order_relaxed);
        g_stats.e_tsc.fetch_add(t1 - t0, std::memory_order_relaxed);
        g_stats.e_cpu.fetch_add(c1 - c0, std::memory_order_relaxed);
    } else {
        g_stats.n_jobs.fetch_add(1, std::memory_order_relaxed);
        g_stats.n_tsc.fetch_add(t1 - t0, std::memory_order_relaxed);
        g_stats.n_cpu.fetch_add(c1 - c0, std::memory_order_relaxed);
    }
}

void run_native_body(Exec& x, bool emu_only) {
    spu::SpuThread& t = *x.t;
    if (!emu_only && whole_ok(t, x.cur)) {
        uint64_t c0 = __rdtsc();
        renjob::WholeResult r;
        if (cfg().spu_host) {
            SpuHostChan h(t, t);
            r = renjob::run_whole(t.ls, t.ctx, h);
        } else {
            FastHost h(t);
            r = renjob::run_whole(t.ls, t.ctx, h);
            if (h.rmw) g_stats.rmw_puts.fetch_add(h.rmw, std::memory_order_relaxed);
            if (h.slow) g_stats.slow_dma.fetch_add(h.slow, std::memory_order_relaxed);
            g_dstats.wr.fetch_add(h.n_wr, std::memory_order_relaxed);
            g_dstats.rd.fetch_add(h.n_rd, std::memory_order_relaxed);
            g_dstats.get.fetch_add(h.n_get, std::memory_order_relaxed);
            g_dstats.put.fetch_add(h.n_put, std::memory_order_relaxed);
            g_dstats.bget.fetch_add(h.b_get, std::memory_order_relaxed);
            g_dstats.bput.fetch_add(h.b_put, std::memory_order_relaxed);
        }
        g_stats.native_cycles.fetch_add(__rdtsc() - c0, std::memory_order_relaxed);
        if (r.done) {
            g_stats.native_done.fetch_add(1, std::memory_order_relaxed);
            t.ctx.pc = kJobRet;
            return;
        }
        g_stats.native_resumed.fetch_add(1, std::memory_order_relaxed);
        g_faillog.note(r.fail_pc, r.icall_miss);
        t.ctx.pc = r.fail_pc;
    }
    emulate_rest(t, nullptr);
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
    t.job_next = cfg().native ? single_next : next_cb;
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
    if (cfg().worker_prio_set) SetThreadPriority(GetCurrentThread(), cfg().worker_prio);
    auto x = make_exec(static_cast<uint32_t>(k), false);
    if (cfg().native) {
        while (fetch(*x, true)) {
            run_native_job(*x);
            finish(*x);
        }
        return;
    }
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
        if (cfg().native) {
            static uint64_t lnd = 0, lnr = 0, lnc = 0, lsd = 0, lrmw = 0, lslow = 0;
            uint64_t nd = g_stats.native_done.load(), nr = g_stats.native_resumed.load(), nc = g_stats.native_cycles.load(), sd = g_stats.state_diff.load(), rmw = g_stats.rmw_puts.load(), sl = g_stats.slow_dma.load();
            LOG_INFO("ppujobs", "5s native: %llu whole, %llu resumed, %.1f us/job native, state diffs %llu, rmw puts %llu, slow dma %llu", static_cast<unsigned long long>(nd - lnd), static_cast<unsigned long long>(nr - lnr),
                     nd - lnd ? double(nc - lnc) / hz * 1e6 / double(nd - lnd) : 0.0, static_cast<unsigned long long>(sd - lsd), static_cast<unsigned long long>(rmw - lrmw), static_cast<unsigned long long>(sl - lslow));
            static uint64_t l[6] = {};
            uint64_t v[6] = {g_stats.n_jobs.load(), g_stats.n_tsc.load(), g_stats.n_cpu.load(), g_stats.e_jobs.load(), g_stats.e_tsc.load(), g_stats.e_cpu.load()};
            uint64_t dn = v[0] - l[0], de = v[3] - l[3];
            LOG_INFO("ppujobs", "5s cost: native %llu jobs %.1f us wall %.1f us cpu; emulated %llu jobs %.1f us wall %.1f us cpu", static_cast<unsigned long long>(dn), dn ? double(v[1] - l[1]) / hz * 1e6 / double(dn) : 0.0,
                     dn ? double(v[2] - l[2]) / hz * 1e6 / double(dn) : 0.0, static_cast<unsigned long long>(de), de ? double(v[4] - l[4]) / hz * 1e6 / double(de) : 0.0, de ? double(v[5] - l[5]) / hz * 1e6 / double(de) : 0.0);
            for (int k = 0; k < 6; k++) l[k] = v[k];
            static uint64_t ld[6] = {};
            uint64_t dv[6] = {g_dstats.wr.load(), g_dstats.rd.load(), g_dstats.get.load(), g_dstats.put.load(), g_dstats.bget.load(), g_dstats.bput.load()};
            double jn = double(nd - lnd ? nd - lnd : 1);
            LOG_INFO("ppujobs", "5s dma per native job: wrch %.0f rdch %.0f, %.0f GETs %.0f bytes, %.0f PUTs %.0f bytes", double(dv[0] - ld[0]) / jn, double(dv[1] - ld[1]) / jn, double(dv[2] - ld[2]) / jn, double(dv[4] - ld[4]) / jn, double(dv[3] - ld[3]) / jn,
                     double(dv[5] - ld[5]) / jn);
            for (int k = 0; k < 6; k++) ld[k] = dv[k];
            lnd = nd;
            lnr = nr;
            lnc = nc;
            lsd = sd;
            lrmw = rmw;
            lslow = sl;
        }
        if (cfg().verify && g_sbx.runs.load()) {
            LOG_INFO("ppujobs", "sandbox verify: %llu runs, %llu same, %llu differ, %llu skipped", static_cast<unsigned long long>(g_sbx.runs.load()), static_cast<unsigned long long>(g_sbx.same.load()),
                     static_cast<unsigned long long>(g_sbx.differ.load()), static_cast<unsigned long long>(g_sbx.skipped.load()));
            std::lock_guard lk(g_sbx_m);
            for (auto& [m, c] : g_sbx_mask) LOG_INFO("ppujobs", "sandbox mask 0x%x: same %llu differ %llu skipped %llu", m, static_cast<unsigned long long>(c[0]), static_cast<unsigned long long>(c[1]), static_cast<unsigned long long>(c[2]));
        }
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
        LOG_INFO("ppujobs", "PPU-level job execution on: %d workers, waiter helps %d, verify every %u, modules %zu, native %d%s", cfg().threads, cfg().help ? 1 : 0, cfg().verify, cfg().mods.size(), cfg().native ? 1 : 0, cfg().spu_host ? " (spu host)" : "");
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
        if (cfg().native) {
            while (ls->pending.load(std::memory_order_acquire) > 0 && fetch(*helper, false)) {
                run_native_job(*helper);
                finish(*helper);
            }
        } else {
            while (ls->pending.load(std::memory_order_acquire) > 0 && g_queued.load(std::memory_order_relaxed)) {
                helper->t->ctx.pc = kJobRet;
                helper->t->exec();
            }
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
