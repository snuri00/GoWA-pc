#include "jobs_whole.h"

#include "runtime/jobsys.h"

namespace wj {

namespace {

uint32_t rbe32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

void wbe32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

uint32_t spu_number(uint32_t worker) {
    if (worker < 16) return worker % 6;
    if (worker < 22) return worker - 16;
    return (5 + 6 - (31 - worker) % 6) % 6;
}

void kernel_context(uint8_t* ls) {
    wbe32(ls + 0x1c0, 0);
    wbe32(ls + 0x1c4, rbe32(ls + 0x1340 + 0xc));
    wbe32(ls + 0x1c8, spu_number(rbe32(ls + 0x1340)));
}

template <int J>
jobsys::WholeResult whole(uint8_t* ls, spu::Context& regs, renjob::ChanHost& host) {
    if (job_in_ls(ls) != J) return jobsys::WholeResult{false, regs.pc, 0};
    kernel_context(ls);
    Result r = run(J, ls, regs, host, false, nullptr);
    return jobsys::WholeResult{r.done, r.fail_pc, r.icall_miss};
}

template <int J>
jobsys::ModuleInfo module(uint32_t flags) {
    jobsys::ModuleInfo m;
    m.ea = info(J).img_ea;
    m.name = info(J).name;
    m.whole = whole<J>;
    m.whole_entry = info(J).entry;
    m.flags = flags;
    return m;
}

const jobsys::Registrar r_pb(module<kPrimBuilder>(0));
const jobsys::Registrar r_gv(module<kGroupVis>(0));
const jobsys::Registrar r_mh(module<kMeshHier>(0));
const jobsys::Registrar r_sh(module<kShadows>(0));
const jobsys::Registrar r_sp(module<kSortPrim>(0));
const jobsys::Registrar r_mp(module<kMeshPrep>(jobsys::kOptIn));

}

}
