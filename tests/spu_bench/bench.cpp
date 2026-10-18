#include "spu/aot.h"
#include "spu/context.h"

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

class Replay : public spu::Host {
public:
    State& s;
    const Capture& cap;
    size_t pos = 0;
    std::string error;
    Replay(State& st, const Capture& c) : s(st), cap(c) {}
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
        if (!e.lines.empty()) {
            uint32_t ep = s.epoch.fetch_add(1) + 1;
            for (auto& [l, d] : e.lines) {
                std::memcpy(s.ls + l, d, 128);
                s.line_epoch[l >> 7].store(ep);
            }
        }
        s.c.pc = e.pc;
        s.c.srr0 = e.srr0;
        s.c.interrupts_enabled = e.ie != 0;
        if (e.halted) s.c.halted = true;
        return &e;
    }
    uint32_t read_channel(uint32_t ch) override {
        const Event* e = next(0, ch, 0, false);
        return e ? e->value : 0;
    }
    void write_channel(uint32_t ch, uint32_t value) override { next(1, ch, value, true); }
    uint32_t channel_count(uint32_t ch) override {
        const Event* e = next(2, ch, 0, false);
        return e ? e->value : 0;
    }
    bool stop_and_signal(uint32_t code) override {
        const Event* e = next(3, code, 0, false);
        return e ? e->result != 0 : false;
    }
    void halt(uint32_t pc, uint32_t word) override { next(4, pc, word, true); }
};

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
        Sleep(0);
        if (SuspendThread(target) == static_cast<DWORD>(-1)) return;
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(target, &c)) g_rips[c.Rip]++;
        ResumeThread(target);
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
    for (size_t i = 0; i < v.size() && i < 30; i++) std::printf("  %5.1f%% %s
", 100.0 * v[i].second / total, v[i].first.c_str());
    std::FILE* f = std::fopen("spu_bench_rips.txt", "w");
    if (f) {
        for (auto& [rip, n] : g_rips) {
            auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            DWORD64 disp = 0;
            if (SymFromAddr(GetCurrentProcess(), rip, &disp, sym)) std::fprintf(f, "%llu %s+0x%llx
", static_cast<unsigned long long>(n), sym->Name, static_cast<unsigned long long>(disp));
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

Result run(State& s, const Capture& cap, bool regions) {
    Result r;
    s.reset(cap);
    Replay host(s, cap);
    std::vector<Slot> cache(spu::LS_SIZE / 4);
    uint64_t n = 0;
    uint64_t t0 = __rdtsc();
    while (n < cap.insns && !s.c.halted) {
        Slot& slot = cache[s.c.pc >> 2];
        const spu::CompiledBlock* b = slot.b;
        if (!b || !s.fresh(s.c.pc, b->words, slot.valid)) {
            uint32_t e = s.epoch.load();
            if (!b || std::memcmp(s.ls + s.c.pc, b->code, b->words * 4) != 0) b = spu::aot::find(s.ls, s.c.pc);
            slot.b = b;
            slot.valid = b ? e : 0;
        }
        r.dispatches++;
        if (!b) {
            spu::step(s.c, host);
            n++;
            r.interpreted++;
            if (host.pos < cap.events.size() && cap.events[host.pos].kind == 5 && cap.events[host.pos].value == n) host.next(5, 0, uint32_t(n), true);
            continue;
        }
        uint32_t ran = b->words;
        bool ok;
        uint64_t b0 = g_profile ? __rdtsc() : 0;
        if (regions && b->region) {
            s.c.executed = 0;
            ok = b->region(s.c, host);
            if (s.c.executed) ran = s.c.executed;
            else ok = b->fn(s.c, host);
        } else {
            ok = b->fn(s.c, host);
        }
        n += ran;
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
    r.cycles = __rdtsc() - t0;
    r.insns = n;
    r.error = host.error;
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
    std::vector<std::string> files;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--reps" && i + 1 < argc) reps = std::atoi(argv[++i]);
        else if (a == "--no-regions") regions = false;
        else if (a == "--profile") g_profile = true;
        else if (a == "--sample") g_sample = true;
        else files.push_back(a);
    }
    State s;
    std::thread sampler_thread;
    if (g_sample) {
        HANDLE self = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
        g_sampling = true;
        sampler_thread = std::thread(sampler, self);
    }
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
        for (int k = 0; k < reps; k++) {
            Result r = run(s, cap, regions);
            if (k == 0 || r.cycles < best.cycles) best = r;
        }
        std::printf("%-28s slot %u insns %9llu events %6zu  %6.2f cycles/insn  %5.1f insns/dispatch  interp %llu  %s\n", f.c_str(), cap.slot, static_cast<unsigned long long>(best.insns),
                    cap.events.size(), best.insns ? double(best.cycles) / best.insns : 0.0, best.dispatches ? double(best.insns) / best.dispatches : 0.0,
                    static_cast<unsigned long long>(best.interpreted), best.exact ? "exact" : best.error.c_str());
        if (!best.exact) failures++;
        total_cycles += best.cycles;
        total_insns += best.insns;
    }
    if (g_sample) {
        g_sampling = false;
        sampler_thread.join();
        report_samples();
    }
    if (g_profile) {
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
    std::printf("total %.3f cycles/insn over %llu insns, %d mismatching\n", total_insns ? double(total_cycles) / total_insns : 0.0, static_cast<unsigned long long>(total_insns), failures);
    return failures ? 1 : 0;
}
