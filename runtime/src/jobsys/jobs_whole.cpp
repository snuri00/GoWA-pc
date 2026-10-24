#include "jobs_whole.h"

#include "wj_an_gen.h"
#include "wj_gv_gen.h"
#include "wj_mh_gen.h"
#include "wj_mp_gen.h"
#include "wj_pb_gen.h"
#include "wj_sh_gen.h"
#include "wj_sp_gen.h"

#include <cstring>
#include <xmmintrin.h>

namespace wj {

namespace {

using RootFn = void (*)(renjob::xo::V*, uint8_t*, renjob::xo::Ctl&, uint32_t);

struct Entry {
    Info info;
    RootFn root;
};

#define WJ_ENTRY(ns, name) {{name, renjob::wj_##ns::kImgEa, renjob::wj_##ns::kImgSize, renjob::wj_##ns::kEntry, renjob::wj_##ns::kTail, renjob::wj_##ns::kSig, renjob::wj_##ns::kEntrySig}, renjob::wj_##ns::root}

const Entry kJobs[kJobCount] = {
    WJ_ENTRY(pb, "PrimBuilder"), WJ_ENTRY(gv, "GroupVis"), WJ_ENTRY(sp, "SortPrim"), WJ_ENTRY(mp, "MeshPrep"),
    WJ_ENTRY(mh, "MeshHierarchy"), WJ_ENTRY(sh, "Shadows"), WJ_ENTRY(an, "Animation"),
};

#undef WJ_ENTRY

uint32_t rbe32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

}

const Info& info(int job) { return kJobs[job].info; }

int job_by_ea(uint32_t img_ea) {
    for (int i = 0; i < kJobCount; i++)
        if (kJobs[i].info.img_ea == img_ea) return i;
    return -1;
}

int job_in_ls(const uint8_t* ls) {
    uint32_t w0 = rbe32(ls + 0x4000);
    for (int i = 0; i < kJobCount; i++) {
        const uint32_t* h = kJobs[i].info.header;
        if (h[0] != w0) continue;
        bool ok = true;
        for (int k = 1; k < 12 && ok; k++) ok = rbe32(ls + 0x4000 + 4 * k) == h[k];
        if (ok) return i;
    }
    return -1;
}

Result run(int job, uint8_t* ls, spu::Context& c, renjob::ChanHost& h, bool load_next_job) {
    constexpr unsigned kWant = 0x6000u | 0x8040u;
    unsigned old = _mm_getcsr();
    if ((old & kWant) != kWant) _mm_setcsr(old | kWant);
    const Entry& e = kJobs[job];
    renjob::xo::Ctl ctl;
    ctl.host = &h;
    ctl.ie = &c.interrupts_enabled;
    t_lnj = load_next_job;
    auto* R = reinterpret_cast<renjob::xo::V*>(c.gpr);
    uint32_t at = e.info.entry;
    Result r;
    for (int k = 0; k < 4; k++) {
        ctl.out = false;
        ctl.fail = false;
        e.root(R, ls, ctl, at);
        uint32_t pc = ctl.fail ? ctl.fail_pc : (renjob::xo::pref(R[0]) & 0x3fffc);
        if (pc == e.info.tail && at != e.info.tail && !h.abort) {
            at = e.info.tail;
            continue;
        }
        if (!ctl.fail && pc == kJobReturn) {
            r.done = true;
            break;
        }
        r.fail_pc = pc;
        r.icall_miss = ctl.icall_miss;
        break;
    }
    if (!r.done && !r.fail_pc) r.fail_pc = renjob::xo::pref(R[0]) & 0x3fffc;
    t_lnj = false;
    if ((old & kWant) != kWant) _mm_setcsr(old);
    return r;
}

}
