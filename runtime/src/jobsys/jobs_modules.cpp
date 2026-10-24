#include "jobs_whole.h"

#include "runtime/jobsys.h"

namespace wj {

namespace {

template <int J>
jobsys::WholeResult whole(uint8_t* ls, spu::Context& regs, renjob::ChanHost& host) {
    if (job_in_ls(ls) != J) return jobsys::WholeResult{false, regs.pc, 0};
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
