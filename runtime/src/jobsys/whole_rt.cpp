#include "jwhole_rt.h"

#include "spu/hle.h"

#include "runtime/log.h"

#include <atomic>

#if __has_include("jwhole_list.h")
#include "jwhole_list.h"
#endif

namespace jobsys::jw {

thread_local spu::Context* t_ctx = nullptr;

namespace {

struct ChanAsHost final : spu::Host {
    renjob::xo::Ctl& ctl;
    bool odd = false;
    explicit ChanAsHost(renjob::xo::Ctl& c) : ctl(c) {}
    uint32_t read_channel(uint32_t ch) override { return ctl.host->rd(ch); }
    void write_channel(uint32_t ch, uint32_t value) override { ctl.host->wr(ch, value); }
    uint32_t channel_count(uint32_t ch) override { return ctl.host->cnt(ch); }
    bool stop_and_signal(uint32_t) override {
        odd = true;
        return false;
    }
    void halt(uint32_t, uint32_t) override { odd = true; }
    bool interrupt_pending() override { return false; }
};

std::atomic<uint64_t> g_hle_calls{0};

}

bool hle_call(renjob::xo::V* R, uint8_t* ls, renjob::xo::Ctl& ctl, uint32_t pc) {
    if (!spu::hle::any_at(pc) || !t_ctx) return false;
    spu::Context& c = *t_ctx;
    if (reinterpret_cast<renjob::xo::V*>(c.gpr) != R || c.ls != ls) return false;
    for (const spu::hle::Function* f = spu::hle::lookup(ls, pc); f; f = spu::hle::lookup_next(ls, pc, f)) {
        c.pc = pc;
        if (!f->host_handler) {
            if (f->handler(c)) {
                g_hle_calls.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            continue;
        }
        ChanAsHost h(ctl);
        c.executed = 0;
        if (f->host_handler(c, h)) {
            if (h.odd) {
                static std::atomic<int> logged{0};
                if (logged.fetch_add(1) < 4) LOG_WARN("jobsys", "host native at 0x%05x stopped or halted inside a whole job", pc);
            }
            g_hle_calls.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

}

namespace jobsys {

namespace {

#ifdef JW_MODULES
#define JW_REG(ea, name, ns) const Registrar reg_##ns({ea, name, nullptr, renjob::ns::run, renjob::ns::kEntry, 0, 0, 0});
JW_MODULES(JW_REG)
#undef JW_REG
#endif

}

}
