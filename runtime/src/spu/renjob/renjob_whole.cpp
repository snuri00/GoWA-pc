#include "renjob_whole.h"

#include "renjob_whole_gen.h"

#include <xmmintrin.h>

namespace renjob {

namespace wh {

#define FAIL(pc) do { ctl.fail = true; ctl.out = true; ctl.fail_pc = pc; return; } while (0)

void pm_gettag(V* R, uint8_t* ls, Ctl& ctl) {
    V& r3 = R[3];
    V& r4 = R[4];
    V& r5 = R[5];
    V& r74 = R[74];
    V& r75 = R[75];
    r4 = pref_only(0x17bc);
    r5 = il(1);
    r74 = lq(ls, 0x1290);
    r3 = nor(r74, r74);
    r75 = ceqi(r74, -1);
    r3 = clz(r3);
    if (pref(r75) != 0) FAIL(0x0295c);
    r3 = sfi(r3, 31);
    r5 = shl(r5, r3);
    r5 = or_(r74, r5);
    stp(ls, 0x1290, r5, ctl);
}

void pm_releasetag(V* R, uint8_t* ls, Ctl& ctl) {
    V& r0 = R[0];
    V& r1 = R[1];
    V& r2 = R[2];
    V& r3 = R[3];
    V& r4 = R[4];
    V& r74 = R[74];
    V& r75 = R[75];
    V& r80 = R[80];
    V& r81 = R[81];
    stp(ls, pref(r1) - 128, r1, ctl);
    stp(ls, pref(r1) + 16, r0, ctl);
    r1 = ai(r1, -128);
    stp(ls, pref(r1) + 32, r80, ctl);
    r80 = ai(r3, 0);
    stp(ls, pref(r1) + 48, r81, ctl);
    r4 = ila(0x1b58);
    r74 = ila(0x1);
    r74 = shl(r74, r3);
    r75 = lq(ls, 0x1290);
    r74 = andc(r75, r74);
    stp(ls, 0x1290, r74, ctl);
    r0 = ila(0x1b68);
    r2 = lq(ls, 0x12a0);
    r2 = ceqhi(r2, 2);
    if (hw6(r2) != 0) FAIL(0x02e60);
    r0 = lq(ls, pref(r1) + 144);
    r80 = lq(ls, pref(r1) + 32);
    r81 = lq(ls, pref(r1) + 48);
    r1 = lq(ls, pref(r1) + 0);
}

#undef FAIL

}

WholeResult run_whole(uint8_t* ls, spu::Context& c, ChanHost& h) {
    static_assert(sizeof(c.gpr) == 128 * sizeof(xo::V));
    constexpr unsigned kWant = 0x6000u | 0x8040u;
    unsigned old = _mm_getcsr();
    if ((old & kWant) != kWant) _mm_setcsr(old | kWant);
    xo::Ctl ctl;
    ctl.host = &h;
    ctl.ie = &c.interrupts_enabled;
    xo::V* R = reinterpret_cast<xo::V*>(c.gpr);
    wh::f_14cf8(R, ls, ctl, kWholeEntry);
    for (int k = 0; k < 4 && !ctl.fail; k++) {
        uint32_t pc = xo::pref(R[0]) & 0x3fffc;
        if (pc == kJobReturn) break;
        if (pc == kWholeTail) {
            ctl.out = false;
            wh::f_14cf8(R, ls, ctl, kWholeTail);
            continue;
        }
        ctl.fail = true;
        ctl.fail_pc = pc;
    }
    if (!ctl.fail && (xo::pref(R[0]) & 0x3fffc) != kJobReturn) {
        ctl.fail = true;
        ctl.fail_pc = xo::pref(R[0]) & 0x3fffc;
    }
    if ((old & kWant) != kWant) _mm_setcsr(old);
    WholeResult r;
    r.done = !ctl.fail;
    r.fail_pc = ctl.fail_pc;
    r.icall_miss = ctl.icall_miss;
    return r;
}

}
