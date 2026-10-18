#include "unit.h"

#include <cstring>

namespace edgeunit {

namespace {

class StrictHost : public spu::Host {
public:
    bool failed = false;
    uint32_t read_channel(uint32_t) override {
        failed = true;
        return 0;
    }
    void write_channel(uint32_t, uint32_t) override { failed = true; }
    uint32_t channel_count(uint32_t) override {
        failed = true;
        return 0;
    }
    bool stop_and_signal(uint32_t) override {
        failed = true;
        return false;
    }
    void halt(uint32_t, uint32_t) override { failed = true; }
};

}

void for_each_call(const std::vector<std::string>& caps, uint32_t fn, const HitFn& cb, const std::function<bool(const uint8_t*)>& image_ok) {
    for (const std::string& path : caps) {
        edgecap::Capture cap;
        if (!cap.load(path)) continue;
        edgecap::Machine m(cap);
        int job = -1;
        bool active = false;
        Hit h;
        size_t pos0 = 0;
        uint64_t n0 = 0;
        while (!m.done()) {
            uint32_t pc = m.c.pc;
            if (pc == 0xad98 && image_ok(m.c.ls)) job++;
            if (!active && pc == fn && image_ok(m.c.ls)) {
                active = true;
                h = Hit{};
                h.cap = path;
                h.job = job;
                h.insn = m.n;
                h.fn = fn;
                h.ret = m.c.gpr[0].u32[3];
                h.sp = m.c.gpr[1].u32[3];
                h.before.ls.assign(m.ls.begin(), m.ls.begin() + spu::LS_SIZE);
                h.before.regs = m.c;
                pos0 = m.pos;
                n0 = m.n;
            }
            m.step();
            if (active && m.c.pc == h.ret && m.c.gpr[1].u32[3] == h.sp) {
                active = false;
                h.fn_insns = m.n - n0;
                h.pure = m.pos == pos0;
                h.after.ls.assign(m.ls.begin(), m.ls.begin() + spu::LS_SIZE);
                h.after.regs = m.c;
                if (!cb(h)) return;
            }
        }
    }
}

IsoResult run_isolated(Snapshot& s, uint32_t ret, uint32_t sp, uint64_t max_insns) {
    IsoResult r;
    s.ls.resize(spu::LS_SIZE + 16);
    s.regs.ls = s.ls.data();
    s.regs.line_epoch = nullptr;
    s.regs.halted = false;
    StrictHost host;
    while (r.insns < max_insns) {
        spu::step(s.regs, host);
        r.insns++;
        if (host.failed) {
            r.error = "channel op";
            return r;
        }
        if (s.regs.pc == ret && s.regs.gpr[1].u32[3] == sp) {
            r.ok = true;
            return r;
        }
    }
    r.error = "timeout";
    return r;
}

LsDiff diff_ls(const uint8_t* a, const uint8_t* b, uint32_t sp, uint32_t stack_window) {
    LsDiff d;
    uint32_t lo = sp > stack_window ? sp - stack_window : 0;
    for (uint32_t i = 0; i < spu::LS_SIZE; i++) {
        if (i >= lo && i < sp) continue;
        if (a[i] != b[i]) {
            if (!d.bytes) d.first = i;
            d.last = i;
            d.bytes++;
        }
    }
    return d;
}

}
