#pragma once

#include "core.h"

#include "spu/renjob/renjob_chan.h"
#include "spu/thread.h"

#include "runtime/memory.h"
#include "runtime/reservation.h"

#include <cstring>

namespace jobsys {

struct MfcStats {
    uint32_t wr = 0, rd = 0, get = 0, put = 0, bget = 0, bput = 0, slow = 0, polls = 0;
};

struct MfcHost final : renjob::ChanHost {
    spu::SpuThread& t;
    JobCtx& c;
    MfcStats st;
    uint32_t table = 0;
    bool mark_all = false;
    uint32_t last_get_ea = 0, last_get_size = 0;
    uint32_t last_llar = 0;
    uint64_t last_llar_ver = 0;
    MfcHost(spu::SpuThread& th, JobCtx& ctx) : t(th), c(ctx) { table = vm::is_mapped(0x9B2298) ? vm::read32(0x9B2298) : 0; }
    bool in_table(uint32_t ea, uint32_t size) const { return table && ea < table + 0xA280 && ea + size > table; }
    uint32_t rd(uint32_t ch) override {
        st.rd++;
        switch (ch) {
        case 13: return t.ctx.interrupts_enabled ? 1 : 0;
        case 24:
            if (t.tag_stat_count) {
                t.tag_stat_count = false;
                return t.tag_stat;
            }
            break;
        case 27:
            if (t.atomic_stat_count) {
                t.atomic_stat_count = false;
                return t.atomic_stat;
            }
            break;
        default: break;
        }
        return t.read_channel(ch);
    }
    uint32_t cnt(uint32_t ch) override {
        switch (ch) {
        case 21: return 16;
        case 23: return 1;
        case 24: return t.tag_stat_count;
        case 27: return t.atomic_stat_count;
        default: return t.channel_count(ch);
        }
    }
    void wr(uint32_t ch, uint32_t v) override {
        st.wr++;
        switch (ch) {
        case 16: t.mfc_lsa = v; return;
        case 17: return;
        case 18: t.mfc_eal = v; return;
        case 19: t.mfc_size = v > 0xFFFF ? 0xFFFF : v; return;
        case 20: t.mfc_tag = v & 0x1F; return;
        case 21: mfc(v & 0xFF); return;
        case 22: t.tag_mask = v; return;
        case 23:
            t.tag_stat = t.tag_mask;
            if (v == 1 && t.tag_mask == 0) return;
            t.tag_stat_count = true;
            return;
        default: t.write_channel(ch, v); return;
        }
    }
    bool plain(uint32_t ea, uint32_t size, uint32_t lsa) const {
        return size && size <= 0x4000 && ea >= 0x10000 && ea < 0xF0000000u && (lsa & 0x3FFFF) + size <= spu::LS_SIZE && vm::is_mapped(ea) && vm::is_mapped(ea + size - 1);
    }
    void get(uint32_t lsa, uint32_t ea, uint32_t size) {
        st.get++;
        st.bget += size;
        lsa &= 0x3FFFF;
        bool changed = true;
        if (vm::is_immutable(ea, size)) {
            std::memcpy(t.ls + lsa, vm::ptr(ea), size);
        } else {
            uint32_t lo = 0xFFFFFFFFu, hi = 0;
            changed = rsv::copy_range_if_changed(ea, t.ls + lsa, size, lo, hi);
        }
        if (mark_all || lsa < 0x17580) t.mark_ls(lsa, size);
        if (ea == last_get_ea && size == last_get_size && !changed && size <= 128) {
            st.polls++;
            c.relax();
        } else {
            c.progress();
        }
        last_get_ea = ea;
        last_get_size = size;
    }
    void put(uint32_t lsa, uint32_t ea, uint32_t size) {
        st.put++;
        st.bput += size;
        lsa &= 0x3FFFF;
        rsv::store_range(ea, t.ls + lsa, size);
        c.progress();
    }
    void slow_path(uint32_t cmd) {
        st.slow++;
        t.write_channel(21, cmd);
    }
    void mfc(uint32_t cmd) {
        uint32_t lsa = t.mfc_lsa, ea = t.mfc_eal, size = t.mfc_size;
        switch (cmd) {
        case 0x40: case 0x41: case 0x42:
            if (!plain(ea, size, lsa) || in_table(ea, size)) return slow_path(cmd);
            get(lsa, ea, size);
            return;
        case 0x20: case 0x21: case 0x22:
            if (!plain(ea, size, lsa) || in_table(ea, size)) return slow_path(cmd);
            put(lsa, ea, size);
            return;
        case 0x44: case 0x45: case 0x46:
        case 0x24: case 0x25: case 0x26: {
            bool is_put = cmd < 0x40;
            uint32_t list = ea & 0x3FFF8;
            uint32_t cur = lsa & 0x3FFF0;
            for (uint32_t n = size, l = list, cc = cur; n >= 8; n -= 8) {
                const uint8_t* e = t.ls + l;
                uint32_t ts = ((uint32_t(e[2]) << 8) | e[3]) & 0x7FFF;
                uint32_t a;
                std::memcpy(&a, e + 4, 4);
                a = __builtin_bswap32(a);
                if ((e[0] & 0x80) || (ts && (!plain(a, ts, cc | (a & 15)) || in_table(a, ts)))) return slow_path(cmd);
                cc = (cc + ((ts + 15) & ~15u)) & 0x3FFF0;
                l = (l + 8) & 0x3FFF8;
                if (l + 8 > spu::LS_SIZE && n > 8) return slow_path(cmd);
            }
            for (uint32_t n = size; n >= 8; n -= 8) {
                const uint8_t* e = t.ls + list;
                uint32_t ts = ((uint32_t(e[2]) << 8) | e[3]) & 0x7FFF;
                uint32_t a;
                std::memcpy(&a, e + 4, 4);
                a = __builtin_bswap32(a);
                if (ts) {
                    if (is_put) put(cur | (a & 15), a, ts);
                    else get(cur | (a & 15), a, ts);
                }
                cur = (cur + ((ts + 15) & ~15u)) & 0x3FFF0;
                list = (list + 8) & 0x3FFF8;
            }
            return;
        }
        case 0xD0: {
            uint32_t line = ea & ~127u;
            if (ea < 0x10000 || !vm::is_mapped(line)) return slow_path(cmd);
            uint64_t ver = rsv::snapshot(line, t.rdata);
            if (line == last_llar && ver == last_llar_ver) {
                st.polls++;
                c.relax();
            }
            last_llar = line;
            last_llar_ver = ver;
            t.raddr = line;
            t.rtime = ver;
            std::memcpy(t.ls + (lsa & 0x3FF80), t.rdata, 128);
            t.atomic_stat = 4;
            t.atomic_stat_count = true;
            return;
        }
        case 0xB4: {
            uint32_t line = ea & ~127u;
            if (ea < 0x10000 || !vm::is_mapped(line)) return slow_path(cmd);
            bool ok = t.raddr == line && rsv::commit_if(line, t.rtime, t.rdata, t.ls + (lsa & 0x3FF80), 0, 128);
            t.raddr = 0;
            t.atomic_stat = ok ? 0 : 1;
            t.atomic_stat_count = true;
            if (ok) {
                c.progress();
                last_llar = 0;
            }
            return;
        }
        case 0xB0: case 0xB8: {
            uint32_t line = ea & ~127u;
            if (ea < 0x10000 || !vm::is_mapped(line)) return slow_path(cmd);
            rsv::store_unconditional(line, t.ls + (lsa & 0x3FF80));
            c.progress();
            last_llar = 0;
            if (cmd == 0xB0) {
                t.atomic_stat = 2;
                t.atomic_stat_count = true;
            }
            return;
        }
        case 0xC0: case 0xC8: case 0xCC: case 0x80: case 0x81:
            std::atomic_thread_fence(std::memory_order_seq_cst);
            return;
        default:
            return slow_path(cmd);
        }
    }
};

struct MfcSpuHost final : spu::Host {
    MfcHost& m;
    explicit MfcSpuHost(MfcHost& h) : m(h) { m.mark_all = true; }
    uint32_t read_channel(uint32_t ch) override { return m.rd(ch); }
    void write_channel(uint32_t ch, uint32_t value) override { m.wr(ch, value); }
    uint32_t channel_count(uint32_t ch) override { return m.cnt(ch); }
    bool stop_and_signal(uint32_t code) override { return m.t.stop_and_signal(code); }
    void halt(uint32_t pc, uint32_t word) override { m.t.halt(pc, word); }
    bool interrupt_pending() override { return m.t.interrupt_pending(); }
};

std::unique_ptr<spu::SpuThread> make_spu(uint32_t index, const char* name);
void emulate(spu::SpuThread& t, spu::Host* host);
void reset_regs(spu::SpuThread& t, const JobDesc& j);

}
