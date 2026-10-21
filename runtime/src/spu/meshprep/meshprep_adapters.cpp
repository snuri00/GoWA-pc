#include "meshprep_adapters.h"

#include "groupvis_gen.h"
#include "meshprep_gen.h"
#include "primbuilder_gen.h"
#include "sortprim_gen.h"

#include <cstring>
#include <xmmintrin.h>

namespace meshprep {

namespace {

const JobInfo kJobs[kJobCount] = {
    {"MeshPrep", 0x74b100, 0x4440, 0x4030, 0x7960, {0x433c9e02u, 0x435e1e82u, 0x43432d02u, 0x4344c382u, 0x00000030u, 0x00004460u, 0x00000e20u, 0x000004b0u, 0xc0dec0deu, 0x00000000u, 0x00000000u, 0x00000005u}},
    {"PrimBuilder", 0x6f8800, 0xce30, 0x104d8, 0x10a00, {0x43555002u, 0x435ff482u, 0x42efed02u, 0x42d4df82u, 0x0000c4d8u, 0x00027920u, 0x00000030u, 0x00000010u, 0xc0dec0deu, 0x00000004u, 0x00000000u, 0x00000005u}},
    {"GroupVis", 0x793500, 0x7080, 0xa978, 0xad40, {0x43b2d002u, 0x43209a82u, 0x4297e502u, 0x43d97b82u, 0x00006978u, 0x00029420u, 0x00000010u, 0x00000020u, 0xc0dec0deu, 0x00000004u, 0x00000000u, 0x00000005u}},
    {"SortPrim", 0x6f4a00, 0x2790, 0x6508, 0x6750, {0x433e6c02u, 0x4295f682u, 0x4335eb02u, 0x43a2fb82u, 0x00002508u, 0x0002b1a0u, 0x00000010u, 0x00000000u, 0xc0dec0deu, 0x00000000u, 0x00000000u, 0x00000005u}},
};

uint32_t rbe32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

thread_local uint32_t t_decline_pc = 0;
thread_local uint32_t t_budget_exits = 0;
uint64_t g_loop_budget = 1ull << 26;

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

renjob::xo::Ctl& thread_ctl() {
    thread_local renjob::xo::JournalStore store;
    thread_local renjob::xo::Ctl ctl;
    if (!ctl.journal) store.attach(ctl);
    return ctl;
}

template <GenFn F, int Job, bool J>
bool run_gen(uint8_t* ls, spu::Context& c) {
    static_assert(sizeof(c.gpr) == 128 * sizeof(renjob::xo::V));
    if (!image_loaded(Job, ls)) return false;
    CsrScope csr;
    if constexpr (J) {
        renjob::xo::Ctl& ctl = thread_ctl();
        ctl.reset(g_loop_budget);
        alignas(16) renjob::xo::V backup[128];
        std::memcpy(backup, c.gpr, sizeof backup);
        F(reinterpret_cast<renjob::xo::V*>(c.gpr), ls, ctl);
        if (ctl.fail) {
            t_decline_pc = ctl.fail_pc;
            ctl.rollback(ls);
            std::memcpy(c.gpr, backup, sizeof backup);
            return false;
        }
        t_budget_exits += ctl.budget == 0;
    } else {
        renjob::xo::Ctl ctl;
        ctl.budget = g_loop_budget;
        F(reinterpret_cast<renjob::xo::V*>(c.gpr), ls, ctl);
        t_budget_exits += ctl.budget == 0;
    }
    return true;
}

#define MP_A(addr, fn, j) {0x##addr, 16, renjob::xo::mp::kSig##addr, run_gen<renjob::xo::mp::fn, kMeshPrep, j != 0>, "MeshPrep." #fn, kMeshPrep},
#define PB_A(addr, fn, j) {0x##addr, 16, renjob::xo::pb::kSig##addr, run_gen<renjob::xo::pb::fn, kPrimBuilder, j != 0>, "PrimBuilder." #fn, kPrimBuilder},
#define GV_A(addr, fn, j) {0x##addr, 16, renjob::xo::gv::kSig##addr, run_gen<renjob::xo::gv::fn, kGroupVis, j != 0>, "GroupVis." #fn, kGroupVis},
#define SP_A(addr, fn, j) {0x##addr, 16, renjob::xo::sp::kSig##addr, run_gen<renjob::xo::sp::fn, kSortPrim, j != 0>, "SortPrim." #fn, kSortPrim},

const Adapter kAdapters[] = {MP_ROOTS_MP(MP_A) MP_ROOTS_PB(PB_A) MP_ROOTS_GV(GV_A) MP_ROOTS_SP(SP_A){0, 0, nullptr, nullptr, nullptr, -1}};

#undef MP_A
#undef PB_A
#undef GV_A
#undef SP_A

}

const JobInfo& job_info(int job) { return kJobs[job]; }

int job_by_name(const char* name) {
    for (int i = 0; i < kJobCount; i++) {
        const char* a = kJobs[i].name;
        size_t k = 0;
        while (a[k] && name[k] && (a[k] | 0x20) == (name[k] | 0x20)) k++;
        if (!a[k] && !name[k]) return i;
    }
    return -1;
}

uint32_t last_decline_pc() { return t_decline_pc; }

uint32_t budget_exits() { return t_budget_exits; }

void set_loop_budget(uint64_t n) { g_loop_budget = n ? n : 1; }

bool image_loaded(int job, const uint8_t* ls) {
    const uint32_t* h = kJobs[job].header;
    for (int i = 0; i < 12; i++)
        if (rbe32(ls + 0x4000 + 4 * i) != h[i]) return false;
    return true;
}

const Adapter* adapters(size_t& count) {
    count = sizeof(kAdapters) / sizeof(kAdapters[0]) - 1;
    return kAdapters;
}

}
