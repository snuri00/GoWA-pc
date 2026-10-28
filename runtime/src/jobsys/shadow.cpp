#include "core.h"
#include "mfc.h"

#include "spu/renjob/renjob_chan.h"
#include "spu/thread.h"

#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/reservation.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace jobsys {

namespace {

struct Capture final : renjob::ChanHost {
    spu::SpuThread& t;
    std::map<uint32_t, uint8_t> writes;
    std::vector<uint32_t> atomic_lines;
    uint32_t table = 0;
    explicit Capture(spu::SpuThread& th) : t(th) { table = vm::is_mapped(0x9B2298) ? vm::read32(0x9B2298) : 0; }
    uint8_t byte(uint32_t ea) {
        auto w = writes.find(ea);
        if (w != writes.end()) return w->second;
        return vm::is_mapped(ea) ? vm::ptr(ea)[0] : 0;
    }
    void load(uint32_t lsa, uint32_t ea, uint32_t n) {
        for (uint32_t k = 0; k < n; k++) t.ls[(lsa + k) & 0x3FFFF] = byte(ea + k);
        t.mark_ls(lsa & 0x3FFFF, n);
    }
    void store(uint32_t lsa, uint32_t ea, uint32_t n) {
        for (uint32_t k = 0; k < n; k++) writes[ea + k] = t.ls[(lsa + k) & 0x3FFFF];
    }
    uint32_t rd(uint32_t ch) override {
        switch (ch) {
        case 13: return t.ctx.interrupts_enabled ? 1 : 0;
        case 24: t.tag_stat_count = false; return t.tag_stat;
        case 27: t.atomic_stat_count = false; return t.atomic_stat;
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
            t.atomic_stat = 4;
            t.atomic_stat_count = true;
            return;
        case 0xB4:
            store(lsa & 0x3FF80, ea & ~127u, 128);
            atomic_lines.push_back(ea & ~127u);
            t.atomic_stat = 0;
            t.atomic_stat_count = true;
            return;
        case 0xB0: case 0xB8:
            store(lsa & 0x3FF80, ea & ~127u, 128);
            atomic_lines.push_back(ea & ~127u);
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

struct Pending {
    uint32_t mod = 0;
    uint32_t dep = 0;
    bool armed = false;
    uint64_t t0 = 0;
    std::map<uint32_t, uint8_t> writes;
    std::vector<uint32_t> skip_lines;
};

std::mutex g_m;
std::deque<Pending> g_pending;
std::atomic<uint64_t> g_seq{0};

struct ModCount {
    uint64_t jobs = 0, same = 0, differ = 0, bytes = 0, diff_bytes = 0, aborted = 0, timeout = 0;
};
std::map<uint32_t, ModCount> g_counts;

void compare(Pending& p) {
    uint32_t diff = 0, first = 0, n = 0;
    for (auto& [ea, v] : p.writes) {
        uint32_t line = ea & ~127u;
        if (std::find(p.skip_lines.begin(), p.skip_lines.end(), line) != p.skip_lines.end()) continue;
        n++;
        if (vm::ptr(ea)[0] != v) {
            if (!diff) first = ea;
            diff++;
        }
    }
    ModCount& c = g_counts[p.mod];
    c.jobs++;
    c.bytes += n;
    c.diff_bytes += diff;
    if (diff) c.differ++;
    else c.same++;
    if (diff && c.differ <= 12) {
        const ModuleInfo* m = find_module(p.mod);
        LOG_ERROR("jobsys", "shadow DIFFERS %s: %u of %u written bytes differ from the WWS run, first 0x%08x (%02x vs %02x), %.2f ms after push", m ? m->name : "?", diff, n, first, p.writes[first], vm::ptr(first)[0],
                  double(__rdtsc() - p.t0) / tsc_hz() * 1e3);
    }
}

void checker() {
    double hz = tsc_hz();
    uint64_t last_report = __rdtsc();
    for (;;) {
        Sleep(1);
        std::lock_guard lk(g_m);
        while (!g_pending.empty()) {
            Pending& p = g_pending.front();
            bool zero = vm::read16(p.dep) == 0;
            if (!zero) p.armed = true;
            bool done = zero && p.armed;
            bool old = double(__rdtsc() - p.t0) / hz > 0.5;
            if (!done && !old) break;
            if (done) compare(p);
            else g_counts[p.mod].timeout++;
            g_pending.pop_front();
        }
        if (double(__rdtsc() - last_report) / hz > 5.0) {
            last_report = __rdtsc();
            std::string s;
            for (auto& [mod, c] : g_counts) {
                const ModuleInfo* m = find_module(mod);
                char b[200];
                std::snprintf(b, sizeof b, " %s jobs %llu same %llu differ %llu bytes %llu diff %llu aborted %llu timeout %llu;", m ? m->name : "?", (unsigned long long)c.jobs, (unsigned long long)c.same, (unsigned long long)c.differ,
                              (unsigned long long)c.bytes, (unsigned long long)c.diff_bytes, (unsigned long long)c.aborted, (unsigned long long)c.timeout);
                s += b;
            }
            if (!s.empty()) LOG_INFO("jobsys", "shadow vs WWS:%s", s.c_str());
        }
    }
}

struct Cfg {
    uint32_t every = 0;
    std::vector<uint32_t> mods;
};

const Cfg& scfg() {
    static const Cfg c = [] {
        Cfg c;
        if (const char* e = std::getenv("GOWA_JOBSYS_SHADOW")) c.every = static_cast<uint32_t>(std::strtoul(e, nullptr, 0));
        if (const char* e = std::getenv("GOWA_JOBSYS_SHADOW_MODS")) {
            std::string s = e;
            size_t pos = 0;
            while (pos < s.size()) {
                size_t end = s.find(',', pos);
                if (end == std::string::npos) end = s.size();
                if (end > pos) c.mods.push_back(static_cast<uint32_t>(std::strtoul(s.substr(pos, end - pos).c_str(), nullptr, 16)));
                pos = end + 1;
            }
        }
        return c;
    }();
    return c;
}

}

bool shadow_module(uint32_t mod) {
    const Cfg& c = scfg();
    return c.every && std::find(c.mods.begin(), c.mods.end(), mod) != c.mods.end();
}

void shadow_job(const JobDesc& j) {
    const Cfg& c = scfg();
    if (!c.every || g_seq.fetch_add(1, std::memory_order_relaxed) % c.every || !j.ndeps) return;
    static std::once_flag once;
    std::call_once(once, [] { std::thread(checker).detach(); });
    Executor& x = thread_executor(29);
    const ModuleInfo* m = find_module(j.module_ea);
    prepare_job(x, j);
    spu::SpuThread& t = *x.t;
    bool armed = vm::read16(j.deps[0]) != 0;
    Capture cap(t);
    unsigned csr = _mm_getcsr();
    _mm_setcsr((csr & ~0x6000u) | 0x6000u | 0x8040u);
    bool ok = false;
    Tier tier = tier_for(j.module_ea);
    if (tier == Tier::Whole && m && m->whole) {
        t_job = &j;
        WholeResult r = m->whole(t.ls, t.ctx, cap);
        t_job = nullptr;
        ok = r.done && !cap.abort;
    } else {
        struct H final : spu::Host {
            Capture& c;
            explicit H(Capture& cc) : c(cc) {}
            uint32_t read_channel(uint32_t ch) override { return c.rd(ch); }
            void write_channel(uint32_t ch, uint32_t value) override { c.wr(ch, value); }
            uint32_t channel_count(uint32_t ch) override { return c.cnt(ch); }
            bool stop_and_signal(uint32_t) override {
                c.abort = true;
                return false;
            }
            void halt(uint32_t, uint32_t) override { c.abort = true; }
            bool interrupt_pending() override { return false; }
        } h(cap);
        for (uint32_t k = 0; k < 64; k++) t.clear_snap(k);
        t.mark_all_ls();
        t.exec_host = &h;
        t.stop_flag.store(false);
        t.exec();
        t.exec_host = nullptr;
        ok = !cap.abort;
    }
    _mm_setcsr(csr);
    x.unmarked = true;
    std::lock_guard lk(g_m);
    if (!ok) {
        g_counts[j.module_ea].aborted++;
        return;
    }
    Pending p;
    p.mod = j.module_ea;
    p.dep = j.deps[0];
    p.armed = armed;
    p.t0 = __rdtsc();
    p.writes = std::move(cap.writes);
    p.skip_lines = std::move(cap.atomic_lines);
    for (uint32_t i = 0; i < j.ndeps; i++) p.skip_lines.push_back(j.deps[i] & ~127u);
    g_pending.push_back(std::move(p));
}

}
