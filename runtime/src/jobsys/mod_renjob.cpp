#include "runtime/jobsys.h"

#include "spu/renjob/renjob_adapters.h"
#include "spu/renjob/renjob_whole.h"

namespace jobsys {

namespace {

WholeResult renjob_whole(uint8_t* ls, spu::Context& regs, renjob::ChanHost& host) {
    if (!renjob::image_loaded(ls)) return WholeResult{false, regs.pc, 0};
    renjob::WholeResult r = renjob::run_whole(ls, regs, host);
    return WholeResult{r.done, r.fail_pc, r.icall_miss};
}

const Registrar reg_renjob({0x705680, "RenJob", nullptr, renjob_whole, renjob::kWholeEntry, 0x19300, 0x38a00, 0});

}

}
