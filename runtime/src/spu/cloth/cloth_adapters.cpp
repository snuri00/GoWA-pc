#include "cloth_adapters.h"

#include "cloth_gen.h"

#include <cstring>
#include <xmmintrin.h>

namespace cloth {

namespace {

const uint32_t kHeader[] = {0x4368de02u, 0x42024a82u, 0x43a7bd02u, 0x4271ab82u, 0x00007f60u, 0x00008400u, 0x000001f0u, 0x00000040u, 0xc0dec0deu, 0x0000000cu, 0x00000000u, 0x00000005u};

uint32_t rbe32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

thread_local uint32_t t_decline_pc = 0;

xo::Ctl& thread_ctl() {
    thread_local xo::JournalStore store;
    thread_local xo::Ctl ctl;
    if (!ctl.journal) store.attach(ctl);
    return ctl;
}

using GenFn = void (*)(xo::V*, uint8_t*, xo::Ctl&);

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

template <GenFn F, bool J>
bool run_gen(uint8_t* ls, spu::Context& c) {
    static_assert(sizeof(c.gpr) == 128 * sizeof(xo::V));
    CsrScope csr;
    spuf::RelaxedScope relaxed(spuf::g_relaxed_mode == 2);
    if constexpr (J) {
        xo::Ctl& ctl = thread_ctl();
        ctl.reset();
        alignas(16) xo::V backup[128];
        std::memcpy(backup, c.gpr, sizeof backup);
        F(reinterpret_cast<xo::V*>(c.gpr), ls, ctl);
        if (ctl.fail) {
            t_decline_pc = ctl.fail_pc;
            ctl.rollback(ls);
            std::memcpy(c.gpr, backup, sizeof backup);
            return false;
        }
    } else {
        xo::Ctl ctl;
        F(reinterpret_cast<xo::V*>(c.gpr), ls, ctl);
    }
    return true;
}

#define CS_GEN(addr, fn, j) {0x##addr, 16, xo::kSig##addr, run_gen<xo::fn, j != 0>, "Cloth." #fn},

const Adapter kAdapters[] = {CLOTH_ROOTS(CS_GEN)};

#undef CS_GEN

}

uint32_t last_decline_pc() { return t_decline_pc; }

bool image_loaded(const uint8_t* ls) {
    for (int i = 0; i < 12; i++)
        if (rbe32(ls + 0x4000 + 4 * i) != kHeader[i]) return false;
    return true;
}

const Adapter* adapters(size_t& count) {
    count = sizeof(kAdapters) / sizeof(kAdapters[0]);
    return kAdapters;
}

}
