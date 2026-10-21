#include "scream_adapters.h"

#include "aux_gen.h"
#include "mp3_gen.h"

#include "runtime/log.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <string>
#include <vector>
#include <xmmintrin.h>

namespace scream {

namespace {

const uint32_t kHeaders[2][12] = {
    {0x43004402u, 0x4352ca82u, 0x43c51702u, 0x43c55d82u, 0x00011a08u, 0x0001c0a0u, 0x00001580u, 0x00001c20u, 0xc0dec0deu, 0x00000028u, 0x00004000u, 0x00000005u},
    {0x43715202u, 0x43cf0682u, 0x422a2f02u, 0x42b22582u, 0x00016a80u, 0x00020bc0u, 0x00001360u, 0x00001910u, 0xc0dec0deu, 0x00000028u, 0x00004000u, 0x00000005u},
};

uint32_t rbe32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

thread_local uint32_t t_decline_pc = 0;

renjob::xo::Ctl& thread_ctl() {
    thread_local renjob::xo::JournalStore store;
    thread_local renjob::xo::Ctl ctl;
    if (!ctl.journal) store.attach(ctl);
    return ctl;
}

using GenFn = void (RJ_ABI *)(renjob::xo::V*, uint8_t*, renjob::xo::Ctl&);

struct CsrScope {
    static constexpr unsigned kWant = 0x6000u | 0x8040u;
    unsigned old;
    CsrScope() : old(_mm_getcsr()) {
        if ((old & kWant) != kWant) _mm_setcsr(old | kWant);
    }
    ~CsrScope() {
        if ((old & kWant) != kWant) _mm_setcsr(old);
    }
};

struct ProfSlot {
    std::string name;
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> cycles{0};
};

const bool g_prof_on = [] {
    const char* e = std::getenv("GOWA_SCREAM_PROF");
    return e && e[0] == '1';
}();
std::mutex g_prof_lock;
std::vector<ProfSlot*> g_prof_slots;
std::atomic<uint64_t> g_prof_last{0};

ProfSlot* prof_slot(int img, uint32_t lsa) {
    ProfSlot* s = new ProfSlot;
    char b[16];
    std::snprintf(b, sizeof b, "%s:%05x", img ? "aux" : "mp3", lsa);
    s->name = b;
    std::lock_guard lk(g_prof_lock);
    g_prof_slots.push_back(s);
    return s;
}

void prof_report(uint64_t now) {
    uint64_t last = g_prof_last.load(std::memory_order_relaxed);
    if (now - last < 20000000000ull || !g_prof_last.compare_exchange_strong(last, now)) return;
    struct Row {
        uint64_t cycles, calls;
        ProfSlot* s;
    };
    std::vector<Row> v;
    uint64_t tot = 0, calls = 0;
    {
        std::lock_guard lk(g_prof_lock);
        for (ProfSlot* s : g_prof_slots) {
            uint64_t cy = s->cycles.exchange(0), n = s->calls.exchange(0);
            calls += n;
            tot += cy;
            v.push_back({cy, n, s});
        }
    }
    std::sort(v.begin(), v.end(), [](const Row& a, const Row& b) { return a.cycles > b.cycles; });
    std::string top;
    for (size_t i = 0; i < v.size() && i < 8; i++) top += " " + v[i].s->name + ":" + std::to_string(v[i].cycles / 1000000) + "M/" + std::to_string(v[i].calls);
    LOG_INFO("scream", "native TSC cycles %lluM in %llu calls over %lluM elapsed (%.3f SPU);%s", static_cast<unsigned long long>(tot / 1000000), static_cast<unsigned long long>(calls), static_cast<unsigned long long>((now - last) / 1000000), last ? double(tot) / double(now - last) : 0.0, top.c_str());
}

template <GenFn F, bool J, int Img>
bool run_gen_body(uint8_t* ls, spu::Context& c);

template <GenFn F, bool J, int Img, uint32_t Lsa>
bool run_gen(uint8_t* ls, spu::Context& c) {
    if (!g_prof_on) [[likely]]
        return run_gen_body<F, J, Img>(ls, c);
    static ProfSlot* slot = prof_slot(Img, Lsa);
    uint64_t t0 = __rdtsc();
    bool ok = run_gen_body<F, J, Img>(ls, c);
    uint64_t t1 = __rdtsc();
    slot->calls.fetch_add(1, std::memory_order_relaxed);
    slot->cycles.fetch_add(t1 - t0, std::memory_order_relaxed);
    prof_report(t1);
    return ok;
}

template <GenFn F, bool J, int Img>
bool run_gen_body(uint8_t* ls, spu::Context& c) {
    static_assert(sizeof(c.gpr) == 128 * sizeof(renjob::xo::V));
    if (!image_loaded(ls, Img)) return false;
    CsrScope csr;
    if constexpr (J) {
        renjob::xo::Ctl& ctl = thread_ctl();
        ctl.reset();
        alignas(16) renjob::xo::V backup[128];
        std::memcpy(backup, c.gpr, sizeof backup);
        F(reinterpret_cast<renjob::xo::V*>(c.gpr), ls, ctl);
        if (ctl.fail) {
            t_decline_pc = ctl.fail_pc;
            ctl.rollback(ls);
            std::memcpy(c.gpr, backup, sizeof backup);
            return false;
        }
    } else {
        renjob::xo::Ctl ctl;
        F(reinterpret_cast<renjob::xo::V*>(c.gpr), ls, ctl);
    }
    return true;
}

#define SC_MP3(addr, fn, j) {0x##addr, 16, renjob::xo::scream_mp3::kSig##addr, run_gen<renjob::xo::scream_mp3::fn, j != 0, 0, 0x##addr>, "ScreamMp3." #fn, 0},
#define SC_AUX(addr, fn, j) {0x##addr, 16, renjob::xo::scream_aux::kSig##addr, run_gen<renjob::xo::scream_aux::fn, j != 0, 1, 0x##addr>, "ScreamAux." #fn, 1},

const Adapter kAdapters[] = {SC_MP3_ROOTS(SC_MP3) SC_AUX_ROOTS(SC_AUX)};

#undef SC_MP3
#undef SC_AUX

}

uint32_t last_decline_pc() { return t_decline_pc; }

bool image_loaded(const uint8_t* ls, int image) {
    const uint32_t* h = kHeaders[image];
    for (int i = 0; i < 12; i++)
        if (rbe32(ls + 0x4000 + 4 * i) != h[i]) return false;
    return true;
}

const Adapter* adapters(size_t& count) {
    count = sizeof(kAdapters) / sizeof(kAdapters[0]);
    return kAdapters;
}

}
