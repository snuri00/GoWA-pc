#include "jobs_inplace.h"

#include "jobs_whole.h"

#include "runtime/log.h"
#include "spu/hle.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <intrin.h>

namespace wj {

namespace {

struct JobStats {
    std::atomic<uint64_t> runs{0}, done{0}, resumed{0}, cycles{0}, vruns{0}, vbad{0}, vstate{0};
    std::atomic<uint32_t> last_fail{0}, last_miss{0};
};

JobStats g_stats[kJobCount];
uint32_t g_verify = 0;
bool g_on[kJobCount] = {};
std::atomic<uint64_t> g_seq[kJobCount];

struct SpuChan final : renjob::ChanHost {
    spu::Host& h;
    spu::Context& c;
    SpuChan(spu::Host& host, spu::Context& ctx) : h(host), c(ctx) {}
    uint32_t rd(uint32_t ch) override {
        uint32_t v = h.read_channel(ch);
        abort = c.halted;
        return v;
    }
    void wr(uint32_t ch, uint32_t v) override {
        h.write_channel(ch, v);
        abort = c.halted;
    }
    uint32_t cnt(uint32_t ch) override { return h.channel_count(ch); }
};

struct Rec {
    int kind;
    uint32_t ch;
    uint32_t value;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> lines;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> outs;
};

bool is_put(uint32_t cmd) { return (cmd >= 0x20 && cmd <= 0x26) || (cmd >= 0x30 && cmd <= 0x36) || cmd == 0xa0 || cmd == 0xa1 || cmd == 0xa2; }

template <class F>
void walk_list(const uint8_t* ls, uint32_t lsa, uint32_t eal, uint32_t size, F&& f) {
    uint32_t cur = lsa & 0x3fff0, list = eal & 0x3fff8;
    for (uint32_t n = size; n >= 8; n -= 8) {
        const uint8_t* e = ls + list;
        uint32_t ts = ((uint32_t(e[2]) << 8) | e[3]) & 0x7fff;
        uint32_t ea;
        std::memcpy(&ea, e + 4, 4);
        ea = __builtin_bswap32(ea);
        f(cur | (ea & 15), ts);
        cur = (cur + ((ts + 15) & ~15u)) & 0x3fff0;
        list = (list + 8) & 0x3fff8;
    }
}

void collect(const uint8_t* ls, uint32_t lsa, uint32_t eal, uint32_t size, uint32_t cmd, Rec* r, bool outs) {
    auto take = [&](std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& v, uint32_t a, uint32_t n) {
        a &= 0x3ffff;
        if (n > 0x40000 - a) n = 0x40000 - a;
        v.push_back({a, std::vector<uint8_t>(ls + a, ls + a + n)});
    };
    if (outs) {
        bool put = is_put(cmd);
        if (put && (cmd & 0xf) >= 4 && (cmd & 0xf) <= 6) walk_list(ls, lsa, eal, size, [&](uint32_t a, uint32_t n) { take(r->outs, a, n); });
        else if (put) take(r->outs, lsa, cmd >= 0xa0 ? 4 : size);
        else if (cmd == 0xb4 || cmd == 0xb0 || cmd == 0xb8) take(r->outs, lsa & ~127u, 128);
        return;
    }
    if (cmd == 0x40 || cmd == 0x41 || cmd == 0x42) take(r->lines, lsa, size);
    else if (cmd == 0xd0) take(r->lines, lsa & ~127u, 128);
    else if (cmd == 0x44 || cmd == 0x45 || cmd == 0x46) walk_list(ls, lsa, eal, size, [&](uint32_t a, uint32_t n) { take(r->lines, a, n); });
}

struct Recorder final : spu::Host {
    spu::Host& h;
    spu::Context& c;
    std::vector<Rec> ev;
    uint32_t lsa = 0, eal = 0, size = 0;
    Recorder(spu::Host& host, spu::Context& ctx) : h(host), c(ctx) {}
    uint32_t read_channel(uint32_t ch) override {
        uint32_t v = h.read_channel(ch);
        ev.push_back({0, ch, v, {}, {}});
        return v;
    }
    void write_channel(uint32_t ch, uint32_t value) override {
        if (ch == 16) lsa = value;
        else if (ch == 18) eal = value;
        else if (ch == 19) size = value;
        Rec r{1, ch, value, {}, {}};
        if (ch == 21) collect(c.ls, lsa, eal, size, value & 0xff, &r, true);
        h.write_channel(ch, value);
        if (ch == 21) collect(c.ls, lsa, eal, size, value & 0xff, &r, false);
        ev.push_back(std::move(r));
    }
    uint32_t channel_count(uint32_t ch) override {
        uint32_t v = h.channel_count(ch);
        ev.push_back({2, ch, v, {}, {}});
        return v;
    }
    bool stop_and_signal(uint32_t code) override {
        ev.push_back({3, code, 0, {}, {}});
        return h.stop_and_signal(code);
    }
    void halt(uint32_t pc, uint32_t word) override {
        ev.push_back({4, pc, word, {}, {}});
        h.halt(pc, word);
    }
};

struct Replay final : renjob::ChanHost {
    const std::vector<Rec>& ev;
    uint8_t* ls;
    size_t pos = 0;
    std::string error;
    uint32_t lsa = 0, eal = 0, size = 0;
    Replay(const std::vector<Rec>& e, uint8_t* l) : ev(e), ls(l) {}
    const Rec* next(int kind, uint32_t ch, uint32_t value, bool check) {
        if (pos >= ev.size() || ev[pos].kind != kind || ev[pos].ch != ch || (check && ev[pos].value != value)) {
            if (error.empty()) {
                char b[200];
                if (pos >= ev.size()) std::snprintf(b, sizeof b, "extra op %zu kind %d ch %u val 0x%x", pos, kind, ch, value);
                else std::snprintf(b, sizeof b, "op %zu: kind %d ch %u val 0x%x, ref kind %d ch %u val 0x%x", pos, kind, ch, value, ev[pos].kind, ev[pos].ch, ev[pos].value);
                error = b;
            }
            abort = true;
            return nullptr;
        }
        return &ev[pos++];
    }
    uint32_t rd(uint32_t ch) override {
        const Rec* e = next(0, ch, 0, false);
        return e ? e->value : 0;
    }
    void wr(uint32_t ch, uint32_t v) override {
        if (ch == 16) lsa = v;
        else if (ch == 18) eal = v;
        else if (ch == 19) size = v;
        const Rec* e = next(1, ch, v, true);
        if (ch != 21 || !e) return;
        for (auto& [a, data] : e->outs) {
            if (std::memcmp(ls + a, data.data(), data.size()) != 0 && error.empty()) {
                size_t k = 0;
                while (k < data.size() && ls[a + k] == data[k]) k++;
                char b[160];
                std::snprintf(b, sizeof b, "output differs at op %zu lsa 0x%05x size 0x%zx first diff +0x%zx ea 0x%08x", pos - 1, a, data.size(), k, eal);
                error = b;
                abort = true;
            }
        }
        for (auto& [a, data] : e->lines) std::memcpy(ls + a, data.data(), data.size());
    }
    uint32_t cnt(uint32_t ch) override {
        const Rec* e = next(2, ch, 0, false);
        return e ? e->value : 0;
    }
};

void verify_run(int job, spu::Context& c, spu::Host& h) {
    JobStats& st = g_stats[job];
    std::vector<uint8_t> ls0(c.ls, c.ls + spu::LS_SIZE);
    spu::v128 regs0[128];
    std::memcpy(regs0, c.gpr, sizeof regs0);
    bool ie0 = c.interrupts_enabled;
    Recorder rec(h, c);
    uint64_t steps = 0;
    while (!c.halted && (c.pc & spu::PC_MASK) != kJobReturn && steps < 400000000ull) {
        if (!spu::step(c, rec)) break;
        steps++;
    }
    c.executed = static_cast<uint32_t>(steps > 0xffffffffull ? 0xffffffffu : steps);
    if (c.halted || (c.pc & spu::PC_MASK) != kJobReturn) return;
    st.vruns++;
    std::unique_ptr<uint8_t[]> ls1(new uint8_t[spu::LS_SIZE]);
    std::memcpy(ls1.get(), ls0.data(), spu::LS_SIZE);
    spu::Context c1;
    std::memcpy(c1.gpr, regs0, sizeof regs0);
    c1.ls = ls1.get();
    c1.interrupts_enabled = ie0;
    Replay rp(rec.ev, ls1.get());
    Result r = run(job, ls1.get(), c1, rp, true);
    std::string err = rp.error;
    if (err.empty() && !r.done) {
        char b[64];
        std::snprintf(b, sizeof b, "native did not finish, fail pc 0x%05x", r.fail_pc);
        err = b;
    }
    if (err.empty() && rp.pos != rec.ev.size()) err = "native consumed " + std::to_string(rp.pos) + " of " + std::to_string(rec.ev.size()) + " channel ops";
    if (!err.empty()) {
        st.vbad++;
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 40) LOG_WARN("wj", "%s verify mismatch: %s (fail 0x%05x miss 0x%05x)", info(job).name, err.c_str(), r.fail_pc, r.icall_miss);
        return;
    }
    std::string d;
    for (int k = 0; k < 128 && d.empty(); k++)
        if (std::memcmp(&c1.gpr[k], &c.gpr[k], 16) != 0) d = "r" + std::to_string(k);
    if (d.empty()) {
        for (uint32_t a = 0; a < spu::LS_SIZE; a += 16) {
            if (std::memcmp(ls1.get() + a, c.ls + a, 16) != 0) {
                char b[64];
                std::snprintf(b, sizeof b, "ls 0x%05x", a);
                d = b;
                break;
            }
        }
    }
    if (!d.empty()) {
        st.vstate++;
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 40) LOG_WARN("wj", "%s verify final state differs at %s", info(job).name, d.c_str());
    }
}

std::mutex g_fail_m;
uint32_t g_fail_seen[kJobCount][16][2];
uint32_t g_fail_n[kJobCount];

void note_fail(int job, uint32_t pc, uint32_t miss) {
    std::lock_guard lk(g_fail_m);
    for (uint32_t i = 0; i < g_fail_n[job]; i++)
        if (g_fail_seen[job][i][0] == pc && g_fail_seen[job][i][1] == miss) return;
    if (g_fail_n[job] >= 16) return;
    g_fail_seen[job][g_fail_n[job]][0] = pc;
    g_fail_seen[job][g_fail_n[job]][1] = miss;
    g_fail_n[job]++;
    LOG_INFO("wj", "%s: new resume point 0x%05x (icall target 0x%05x)", info(job).name, pc, miss);
}

template <int J>
bool handler(spu::Context& c, spu::Host& h) {
    if (!g_on[J] || job_in_ls(c.ls) != J) return false;
    JobStats& st = g_stats[J];
    st.runs++;
    if (g_verify && g_seq[J].fetch_add(1, std::memory_order_relaxed) % g_verify == 0) {
        verify_run(J, c, h);
        return true;
    }
    uint64_t t0 = __rdtsc();
    SpuChan ch(h, c);
    Result r = run(J, c.ls, c, ch, true, &h);
    st.cycles += __rdtsc() - t0;
    c.executed = 4096;
    if (r.done) {
        st.done++;
        c.pc = kJobReturn;
    } else {
        st.resumed++;
        st.last_fail.store(r.fail_pc, std::memory_order_relaxed);
        if (r.icall_miss) st.last_miss.store(r.icall_miss, std::memory_order_relaxed);
        note_fail(J, r.fail_pc, r.icall_miss);
        c.pc = r.fail_pc & spu::PC_MASK;
    }
    return true;
}

using HostFn = bool (*)(spu::Context&, spu::Host&);
const HostFn kHandlers[kJobCount] = {handler<0>, handler<1>, handler<2>, handler<3>, handler<4>, handler<5>, handler<6>, handler<7>, handler<8>};
const char* const kShort[kJobCount] = {"pb", "gv", "sp", "mp", "mh", "sh", "an", "sm", "sa"};

void stats_thread() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        std::string line;
        for (int j = 0; j < kJobCount; j++) {
            JobStats& s = g_stats[j];
            uint64_t n = s.runs.exchange(0);
            if (!n) continue;
            uint64_t d = s.done.exchange(0), rs = s.resumed.exchange(0), cy = s.cycles.exchange(0);
            uint64_t vr = s.vruns.exchange(0), vb = s.vbad.exchange(0), vs = s.vstate.exchange(0);
            char b[220];
            std::snprintf(b, sizeof b, " [%s n%llu done%llu res%llu last 0x%05x miss 0x%05x %.0fkcyc/job v%llu bad%llu st%llu]", kShort[j], (unsigned long long)n, (unsigned long long)d,
                          (unsigned long long)rs, s.last_fail.load(), s.last_miss.load(), d ? double(cy) / double(d) / 1000.0 : 0.0, (unsigned long long)vr, (unsigned long long)vb,
                          (unsigned long long)vs);
            line += b;
        }
        uint64_t steps = 0, calls = ext_stats(steps);
        if (!line.empty()) LOG_INFO("wj", "10s:%s ext %llu calls %llu steps", line.c_str(), (unsigned long long)calls, (unsigned long long)steps);
    }
}

}

bool selected(int job) { return job >= 0 && job < kJobCount && g_on[job]; }

void install_inplace() {
    const char* e = std::getenv("GOWA_WJ");
    if (!e || !*e || e[0] == '0') return;
    std::string sel(e);
    for (int j = 0; j < kJobCount; j++) g_on[j] = sel == "1" || sel == "all" || sel.find(kShort[j]) != std::string::npos;
    if (const char* v = std::getenv("GOWA_WJ_VERIFY")) g_verify = static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
    for (int j = 0; j < kJobCount; j++) {
        if (!g_on[j]) continue;
        const Info& in = info(j);
        spu::hle::register_function({in.entry, 16, in.entry_sig, nullptr, in.name, kHandlers[j]});
    }
    std::thread(stats_thread).detach();
}

}
