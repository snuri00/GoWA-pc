#include "edge_exec.h"
#include "edge_hle.h"
#include "runtime/host.h"

#include "../spu/hle.h"
#include "../spu/thread.h"

#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/reservation.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include <immintrin.h>

namespace jsedge {

namespace {

const bool g_prof = [] {
    const char* e = std::getenv("GOWA_JOBSYS_EDGE_PROF");
    return e && *e && e[0] != '0';
}();

struct ProfEntry {
    std::atomic<const char*> name{nullptr};
    std::atomic<uint64_t> ticks{0}, calls{0};
};

ProfEntry g_prof_tab[512];
thread_local uint64_t t_dma = 0;

void prof_add_impl(const char* name, uint64_t ticks) {
    uint32_t h = static_cast<uint32_t>((reinterpret_cast<uintptr_t>(name) >> 3) * 2654435761u) & 511;
    for (uint32_t i = 0; i < 512; i++) {
        ProfEntry& e = g_prof_tab[(h + i) & 511];
        const char* cur = e.name.load(std::memory_order_relaxed);
        if (!cur) {
            if (!e.name.compare_exchange_strong(cur, name)) {
                if (cur != name) continue;
            }
        } else if (cur != name) {
            continue;
        }
        e.ticks.fetch_add(ticks, std::memory_order_relaxed);
        e.calls.fetch_add(1, std::memory_order_relaxed);
        return;
    }
}

struct DmaTimer {
    uint64_t t0;
    DmaTimer() : t0(g_prof ? __rdtsc() : 0) {}
    ~DmaTimer() {
        if (g_prof) t_dma += __rdtsc() - t0;
    }
};

uint32_t be32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

void wbe32(uint8_t* p, uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(p, &v, 4);
}

void set_quad(spu::v128& r, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
    r.u32[3] = w0;
    r.u32[2] = w1;
    r.u32[1] = w2;
    r.u32[0] = w3;
}

bool plain(uint32_t ea, uint32_t size, uint32_t lsa) {
    return size && size <= 0x4000 && ea >= 0x10000 && ea < 0xF0000000u && (lsa & 0x3FFFF) + size <= spu::LS_SIZE && vm::is_mapped(ea) && vm::is_mapped(ea + size - 1);
}

struct FastHost final : spu::Host {
    spu::SpuThread& t;
    HostCounters& hc;
    uint32_t dec_run = 0;
    FastHost(spu::SpuThread& th, HostCounters& c) : t(th), hc(c) {}
    uint32_t read_channel(uint32_t ch) override {
        switch (ch) {
        case 8:
            hc.dec_reads++;
            if ((++dec_run & 127) == 0) host::yield();
            return 0u - static_cast<uint32_t>(__rdtsc() >> 5);
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
        hc.other_reads++;
        return t.read_channel(ch);
    }
    uint32_t channel_count(uint32_t ch) override {
        switch (ch) {
        case 21: return 16;
        case 23: return 1;
        case 24: return t.tag_stat_count;
        case 27: return t.atomic_stat_count;
        default: return t.channel_count(ch);
        }
    }
    bool stop_and_signal(uint32_t code) override { return t.stop_and_signal(code); }
    void halt(uint32_t pc, uint32_t word) override { t.halt(pc, word); }
    bool interrupt_pending() override { return false; }
    void write_channel(uint32_t ch, uint32_t v) override {
        switch (ch) {
        case 16: t.mfc_lsa = v; return;
        case 17: return;
        case 18: t.mfc_eal = v; return;
        case 19: t.mfc_size = v > 0xFFFF ? 0xFFFF : v; return;
        case 20: t.mfc_tag = v & 0x1F; return;
        case 21: dec_run = 0; mfc(v & 0xFF); return;
        case 22: t.tag_mask = v; return;
        case 23:
            t.tag_stat = t.tag_mask;
            if (v == 1 && t.tag_mask == 0) return;
            t.tag_stat_count = true;
            return;
        default: t.write_channel(ch, v); return;
        }
    }
    void get(uint32_t lsa, uint32_t ea, uint32_t size) {
        DmaTimer dt;
        hc.gets++;
        if (ea - 0x50300c80u < 0xa0u) hc.label_gets++;
        hc.get_bytes += size;
        lsa &= 0x3FFFF;
        if (vm::is_immutable(ea, size)) {
            if (std::memcmp(t.ls + lsa, vm::ptr(ea), size) != 0) {
                std::memcpy(t.ls + lsa, vm::ptr(ea), size);
                t.mark_ls(lsa, size);
            }
            return;
        }
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        if (rsv::copy_range_if_changed(ea, t.ls + lsa, size, lo, hi)) t.mark_ls(lsa + lo, hi - lo);
    }
    void put(uint32_t lsa, uint32_t ea, uint32_t size) {
        DmaTimer dt;
        hc.puts++;
        hc.put_bytes += size;
        rsv::store_range(ea, t.ls + (lsa & 0x3FFFF), size);
    }
    void slow_path(uint32_t cmd) {
        hc.slow++;
        t.write_channel(21, cmd);
    }
    void mfc(uint32_t cmd) {
        uint32_t lsa = t.mfc_lsa, ea = t.mfc_eal, size = t.mfc_size;
        switch (cmd) {
        case 0x40: case 0x41: case 0x42:
            if (!size) return;
            if (!plain(ea, size, lsa)) return slow_path(cmd);
            get(lsa, ea, size);
            return;
        case 0x20: case 0x21: case 0x22:
            if (!size) return;
            if (!plain(ea, size, lsa)) return slow_path(cmd);
            put(lsa, ea, size);
            return;
        case 0x44: case 0x45: case 0x46:
        case 0x24: case 0x25: case 0x26: {
            bool is_put = cmd < 0x40;
            uint32_t list = ea & 0x3FFF8;
            uint32_t cur = lsa & 0x3FFF0;
            for (uint32_t n = size, l = list, c = cur; n >= 8; n -= 8) {
                const uint8_t* e = t.ls + l;
                uint32_t ts = ((uint32_t(e[2]) << 8) | e[3]) & 0x7FFF;
                uint32_t a = be32(e + 4);
                if ((e[0] & 0x80) || (ts && !plain(a, ts, c | (a & 15)))) return slow_path(cmd);
                c = (c + ((ts + 15) & ~15u)) & 0x3FFF0;
                l = (l + 8) & 0x3FFF8;
                if (l + 8 > spu::LS_SIZE && n > 8) return slow_path(cmd);
            }
            for (uint32_t n = size; n >= 8; n -= 8) {
                const uint8_t* e = t.ls + list;
                uint32_t ts = ((uint32_t(e[2]) << 8) | e[3]) & 0x7FFF;
                uint32_t a = be32(e + 4);
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
            hc.atomics++;
            uint64_t ver = rsv::snapshot(line, t.rdata);
            t.raddr = line;
            t.rtime = ver;
            std::memcpy(t.ls + (lsa & 0x3FF80), t.rdata, 128);
            t.mark_ls(lsa & 0x3FF80, 128);
            t.atomic_stat = 4;
            t.atomic_stat_count = true;
            return;
        }
        case 0xB4: {
            uint32_t line = ea & ~127u;
            if (ea < 0x10000 || !vm::is_mapped(line)) return slow_path(cmd);
            hc.atomics++;
            bool ok = t.raddr == line && rsv::commit_if(line, t.rtime, t.rdata, t.ls + (lsa & 0x3FF80), 0, 128);
            t.raddr = 0;
            t.atomic_stat = ok ? 0 : 1;
            t.atomic_stat_count = true;
            return;
        }
        case 0xB0: case 0xB8: {
            uint32_t line = ea & ~127u;
            if (ea < 0x10000 || !vm::is_mapped(line)) return slow_path(cmd);
            hc.atomics++;
            rsv::store_unconditional(line, t.ls + (lsa & 0x3FF80));
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

struct SandboxHost final : spu::Host {
    spu::SpuThread& t;
    Sandbox& sb;
    SandboxHost(spu::SpuThread& th, Sandbox& s) : t(th), sb(s) {}
    uint8_t byte(uint32_t ea) {
        auto w = sb.writes.find(ea);
        if (w != sb.writes.end()) return w->second;
        return vm::is_mapped(ea) ? vm::ptr(ea)[0] : 0;
    }
    void load(uint32_t lsa, uint32_t ea, uint32_t n) {
        for (uint32_t k = 0; k < n; k++) t.ls[(lsa + k) & 0x3FFFF] = byte(ea + k);
        t.mark_ls(lsa & 0x3FFFF, n);
    }
    void store(uint32_t lsa, uint32_t ea, uint32_t n) {
        for (uint32_t k = 0; k < n; k++) sb.writes[ea + k] = t.ls[(lsa + k) & 0x3FFFF];
    }
    void fail(uint32_t ch) {
        if (!sb.abort) sb.abort_ch = ch;
        sb.abort = true;
        t.stop_flag.store(true);
        t.ctx.halted = true;
    }
    uint32_t read_channel(uint32_t ch) override {
        switch (ch) {
        case 13: return t.ctx.interrupts_enabled ? 1 : 0;
        case 24:
            t.tag_stat_count = false;
            return t.tag_stat;
        case 27:
            t.atomic_stat_count = false;
            return t.atomic_stat;
        case 8: return 0;
        default: fail(0x100 | ch); return 0;
        }
    }
    uint32_t channel_count(uint32_t ch) override {
        switch (ch) {
        case 21: return 16;
        case 23: return 1;
        case 24: return t.tag_stat_count;
        case 27: return t.atomic_stat_count;
        default: fail(0x200 | ch); return 0;
        }
    }
    bool stop_and_signal(uint32_t code) override {
        fail(0x400 | (code & 0xff));
        return false;
    }
    void halt(uint32_t, uint32_t) override { fail(0x800); }
    bool interrupt_pending() override { return false; }
    void write_channel(uint32_t ch, uint32_t v) override {
        switch (ch) {
        case 16: t.mfc_lsa = v; return;
        case 17: return;
        case 18: t.mfc_eal = v; return;
        case 19: t.mfc_size = v > 0xFFFF ? 0xFFFF : v; return;
        case 20: t.mfc_tag = v & 0x1F; return;
        case 22: t.tag_mask = v; return;
        case 23:
            t.tag_stat = t.tag_mask;
            if (v == 1 && t.tag_mask == 0) return;
            t.tag_stat_count = true;
            return;
        case 21: break;
        default: fail(0x1000 | ch); return;
        }
        uint32_t cmd = v & 0xFF, lsa = t.mfc_lsa & 0x3FFFF, ea = t.mfc_eal, size = t.mfc_size;
        switch (cmd) {
        case 0x40: case 0x41: case 0x42: load(lsa, ea, size); return;
        case 0x20: case 0x21: case 0x22: store(lsa, ea, size); return;
        case 0x44: case 0x45: case 0x46: case 0x24: case 0x25: case 0x26: {
            uint32_t cur = lsa & 0x3FFF0, list = ea & 0x3FFF8;
            for (uint32_t n = size; n >= 8; n -= 8) {
                const uint8_t* e = t.ls + list;
                uint32_t ts = ((uint32_t(e[2]) << 8) | e[3]) & 0x7FFF;
                uint32_t a = be32(e + 4);
                if (cmd < 0x40) store(cur | (a & 15), a, ts);
                else load(cur | (a & 15), a, ts);
                cur = (cur + ((ts + 15) & ~15u)) & 0x3FFF0;
                list = (list + 8) & 0x3FFF8;
            }
            return;
        }
        case 0xD0:
            load(lsa & 0x3FF80, ea & ~127u, 128);
            t.atomic_stat = 4;
            t.atomic_stat_count = true;
            return;
        case 0xB4:
            sb.atomics++;
            store(lsa & 0x3FF80, ea & ~127u, 128);
            t.atomic_stat = 0;
            t.atomic_stat_count = true;
            return;
        case 0xB0: case 0xB8:
            sb.atomics++;
            store(lsa & 0x3FF80, ea & ~127u, 128);
            if (cmd == 0xB0) {
                t.atomic_stat = 2;
                t.atomic_stat_count = true;
            }
            return;
        case 0xC0: case 0xC8: case 0xCC: case 0x80: case 0x81: return;
        default: fail(0x2000 | cmd); return;
        }
    }
};

bool trap(spu::SpuThread& t) {
    ApiState* st = static_cast<ApiState*>(t.job_user);
    if (!st) return false;
    if (api_call(t.ctx, *st)) return true;
    return false;
}

bool no_next(spu::SpuThread&) { return false; }

enum class Fast { Done, Fallback, Failed };

Fast fast_loop(spu::SpuThread& t, spu::Host& h, ApiState& api, std::vector<const spu::hle::Function*>& cache, std::vector<uint8_t>& valid, uint64_t& dispatches) {
    spu::Context& c = t.ctx;
    for (uint32_t guard = 0; guard < 4000000; guard++) {
        uint32_t pc = c.pc & spu::PC_MASK;
        c.pc = pc;
        if (pc == kJobRet) return Fast::Done;
        if (pc < 0x4000) {
            uint64_t a0 = g_prof ? __rdtsc() : 0;
            if (!api_call(c, api)) return Fast::Fallback;
            if (g_prof) prof_add_impl("jobapi", __rdtsc() - a0);
            continue;
        }
        dispatches++;
        const spu::hle::Function* f = nullptr;
        if (pc < 0x10430) {
            uint32_t k = pc >> 2;
            if (!valid[k]) {
                cache[k] = spu::hle::any_at(pc) ? spu::hle::lookup(t.ls, pc) : nullptr;
                valid[k] = 1;
            }
            f = cache[k];
        } else {
            if (spu::hle::any_at(pc)) f = spu::hle::lookup(t.ls, pc);
            if (!f && spu::hle::dyn_at(pc)) f = spu::hle::lookup_dyn(t.ls, pc);
        }
        if (!f) return Fast::Fallback;
        if (!f->host_handler) {
            uint64_t s0 = g_prof ? __rdtsc() : 0;
            if (f->handler(c)) {
                if (g_prof) prof_add_impl(f->name, __rdtsc() - s0);
                c.pc = c.gpr[0].u32[3] & spu::PC_MASK;
                continue;
            }
            f = spu::hle::lookup_next(t.ls, pc, f);
            if (f && !f->host_handler) f = nullptr;
            if (!f) return Fast::Fallback;
        }
        c.executed = 0;
        uint64_t h0 = g_prof ? __rdtsc() : 0, d0 = t_dma;
        if (!f->host_handler(c, h)) {
            c.pc = pc;
            return Fast::Fallback;
        }
        if (g_prof) prof_add_impl(f->name, __rdtsc() - h0 - (t_dma - d0));
        if (c.halted) return Fast::Failed;
        c.irq_check = false;
    }
    return Fast::Fallback;
}

const uint8_t kApiTemplate[0x100] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00, 0x00, 0x30, 0x01, 0xad, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7b, 0xd7, 0xb9, 0xfb, 0x00, 0x02, 0x01, 0xff,
    0x00, 0x00, 0x0c, 0x40, 0x00, 0x00, 0x0c, 0x40, 0x00, 0x00, 0x0c, 0x40, 0x00, 0x00, 0x0c, 0x40,
    0x00, 0x00, 0x16, 0xf0, 0x00, 0x00, 0x2f, 0xd8, 0x00, 0x00, 0x17, 0x60, 0x00, 0x00, 0x17, 0xb8,
    0x00, 0x00, 0x17, 0xc0, 0x00, 0x00, 0x17, 0xe0, 0x00, 0x00, 0x1a, 0xf0, 0x00, 0x00, 0x1b, 0x10,
    0x00, 0x00, 0x1a, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x03, 0xff, 0xff, 0xff, 0xff, 0x00, 0x7a, 0x7b, 0x00, 0x00, 0x00, 0x30, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb9,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00, 0x1d,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
};

}

bool prof_on() { return g_prof; }

void prof_add(const char* name, uint64_t ticks) { prof_add_impl(name, ticks); }

bool parse(uint32_t cmd_ea, uint32_t cmd_size, Cmd& c) {
    if (cmd_size < 0x18 || cmd_size > 0x100 || (cmd_ea & 15) || !vm::range_mapped(cmd_ea, cmd_size)) return false;
    c = Cmd{};
    c.ea = cmd_ea;
    c.size = cmd_size;
    std::memcpy(c.raw, vm::ptr(cmd_ea), cmd_size);
    for (uint32_t off = 0; off + 8 <= cmd_size; off += 8) {
        uint32_t hi = be32(c.raw + off), lo = be32(c.raw + off + 4);
        if (!hi && !lo) continue;
        uint32_t op = (hi >> 9) & 7, set = (hi >> 5) & 15;
        switch (op) {
        case 1: {
            if (set > 3 || c.sets[set].reserved) return false;
            BufSet& s = c.sets[set];
            s.reserved = true;
            s.first_page = lo >> 24;
            s.pages = (lo >> 16) & 0xff;
            s.buffers = (lo >> 8) & 0xff;
            if (!s.pages || !s.buffers || (s.first_page + s.pages * s.buffers) > 0x100) return false;
            break;
        }
        case 2: {
            if (set > 3 || !c.sets[set].reserved || c.sets[set].used || (hi & 0x1f)) return false;
            BufSet& s = c.sets[set];
            s.used = true;
            s.len = (hi >> 18) << 4;
            s.ea = lo;
            s.flags = hi & 0x3f000;
            if (s.len) {
                if (!vm::range_mapped(s.ea, s.len) || s.len > s.pages * 0x400u) return false;
                if (s.flags & 0x4000) {
                    if ((s.ea & 7) || (s.len & 7) || s.len > 0x800) return false;
                } else if ((s.ea & 15) || set != 0 || c.module) {
                    return false;
                } else {
                    c.module = s.ea;
                    c.code_len = s.len;
                }
            }
            break;
        }
        case 3: break;
        case 4:
            if (c.ndeps >= 4 || !lo || !vm::range_mapped(lo & ~127u, 128)) return false;
            c.deps[c.ndeps++] = lo;
            break;
        case 5:
            if (!c.module) return false;
            c.run_off = off;
            c.param_off = (off + 8 + 15) & ~15u;
            if (c.param_off > cmd_size) return false;
            if (vm::read32(c.module + 0x20) != 0xc0dec0de) return false;
            return true;
        default:
            return false;
        }
    }
    return false;
}

bool edge_shape(const Cmd& c) {
    if (c.module != kEdgeEa || c.code_len != 0xd000 || c.ndeps) return false;
    const BufSet& s0 = c.sets[0];
    const BufSet& s1 = c.sets[1];
    const BufSet& s2 = c.sets[2];
    if (s0.first_page != 0x10 || s0.pages != 0x34 || s0.buffers != 1) return false;
    if (s1.first_page != 0x44 || s1.pages != 0x30 || s1.buffers != 2 || !s1.used || !(s1.flags & 0x4000) || !s1.len) return false;
    if (s2.first_page != 0xa4 || s2.pages != 0x48 || s2.buffers != 1 || s2.len) return false;
    if (c.sets[3].reserved) return false;
    if (c.param_off != 0x40) return false;
    const uint8_t* l = vm::ptr(s1.ea);
    uint32_t total = 0;
    for (uint32_t n = 0; n < s1.len; n += 8) {
        if (l[n] & 0x80) return false;
        uint32_t ts = ((uint32_t(l[n + 2]) << 8) | l[n + 3]) & 0x7FFF;
        uint32_t ea = be32(l + n + 4);
        if (ts && !vm::range_mapped(ea, ts)) return false;
        total += (ts + 15) & ~15u;
    }
    return total + ((s1.len + 15) & ~15u) <= 0x30 * 0x400u;
}

uint32_t set_base(const Cmd& c, uint32_t set, uint32_t buf) {
    const BufSet& s = c.sets[set];
    uint32_t b = s.buffers > 1 ? buf % s.buffers : 0;
    return (s.first_page + b * s.pages) * 0x400u;
}

void prepare_ls(uint8_t* ls, spu::Context& ctx, const JobRef& j, const Cmd& c, uint32_t spu_num, uint32_t buf, bool load_code, bool load_pm) {
    if (load_pm) std::memcpy(ls + 0xa00, vm::ptr(kPmEa), 0x3000);
    wbe32(ls + 0x1c0, 0);
    wbe32(ls + 0x1c4, j.spurs);
    wbe32(ls + 0x1c8, spu_num);
    wbe32(ls + 0x1cc, 0x1f);
    wbe32(ls + 0x1d0, 0);
    wbe32(ls + 0x1d4, kPmEa);
    wbe32(ls + 0x1d8, 2);
    wbe32(ls + 0x1dc, j.wid);
    if (load_code) std::memcpy(ls + 0x4000, vm::ptr(c.module), c.code_len);
    std::memcpy(ls + kCmdLs, c.raw, c.size);
    std::memset(ls + kCmdLs, 0, c.run_off);
    uint8_t* api = ls + kApiLs;
    std::memcpy(api, kApiTemplate, sizeof kApiTemplate);
    wbe32(api + 0x00, spu_num);
    wbe32(api + 0x0c, j.spurs);
    wbe32(api + 0x10, (j.wid << 16) | (j.index & 0xffff));
    for (int k = 0; k < 4; k++) wbe32(api + 0x20 + 4 * k, kCmdLs + c.param_off);
    wbe32(api + 0x70, j.hdr | j.block);
    wbe32(api + 0x78, j.index);
    wbe32(api + 0x80, j.index);
    if (j.arr) std::memcpy(api + 0xc0, vm::ptr(j.arr + ((8 * j.index) & ~15u)), 16);
    for (uint32_t set = 0; set < 4; set++) {
        const BufSet& s = c.sets[set];
        if (!s.used || !s.len || !(s.flags & 0x4000)) continue;
        uint32_t base = set_base(c, set, buf);
        uint32_t list_ls = base + s.pages * 0x400u - ((s.len + 15) & ~15u);
        std::memcpy(ls + list_ls, vm::ptr(s.ea), s.len);
        uint32_t cur = base & 0x3FFF0;
        for (uint32_t n = 0; n + 8 <= s.len; n += 8) {
            const uint8_t* e = ls + list_ls + n;
            uint32_t ts = ((uint32_t(e[2]) << 8) | e[3]) & 0x7FFF;
            uint32_t ea = be32(e + 4);
            if (ts) std::memcpy(ls + (cur | (ea & 15)), vm::ptr(ea), ts);
            cur = (cur + ((ts + 15) & ~15u)) & 0x3FFF0;
        }
    }
    auto& r = ctx.gpr;
    set_quad(r[0], kJobRet, 0, 0, 0);
    set_quad(r[1], 0x3f990, 0x1980, 0, 0);
    set_quad(r[3], kApiLs, kApiLs, kApiLs, kApiLs);
    set_quad(r[4], 0, 3, 0, 3);
    set_quad(r[5], 0x4000 + vm::read32(c.module + 0x10), vm::read32(c.module + 0x14), vm::read32(c.module + 0x18), vm::read32(c.module + 0x1c));
    set_quad(r[6], 0x1000, 0, 0, 0);
    ctx.pc = (0x4000 + vm::read32(c.module + 0x10)) & spu::PC_MASK;
    ctx.interrupts_enabled = true;
    ctx.halted = false;
    ctx.irq_check = false;
}

bool api_call(spu::Context& ctx, ApiState& st) {
    uint32_t pc = ctx.pc;
    auto alloc = [&](uint32_t out[4]) {
        for (int k = 0; k < 4; k++) {
            uint32_t m = st.mask[k];
            uint32_t t = m == 0xffffffffu ? 0 : 31 - static_cast<uint32_t>(__builtin_clz(~m));
            st.mask[k] = m | (1u << t);
            out[k] = t;
        }
    };
    switch (pc) {
    case 0x2fd8: {
        uint32_t set = ctx.gpr[4].u32[3];
        if (set > 3 || !st.cmd) break;
        const BufSet& s = st.cmd->sets[set];
        if (!st.tag[set] && ctx.gpr[6].u32[3]) {
            uint32_t t[4];
            alloc(t);
            st.tag[set] = t[0] + 1;
        }
        uint32_t page = s.first_page + (s.buffers > 1 ? (st.buf % s.buffers) * s.pages : 0);
        set_quad(ctx.gpr[3], (set << 24) | (st.tag[set] ? st.tag[set] - 1 : 0), (page << 24) | (s.pages << 8), s.ea, s.len);
        break;
    }
    case 0x17b8: {
        uint32_t t[4];
        alloc(t);
        set_quad(ctx.gpr[3], t[0], t[1], t[2], t[3]);
        break;
    }
    case 0x17c0: {
        const spu::v128& q = ctx.gpr[3];
        uint32_t t[4] = {q.u32[3], q.u32[2], q.u32[1], q.u32[0]};
        for (int k = 0; k < 4; k++) st.mask[k] &= ~(1u << (t[k] & 31));
        break;
    }
    case 0x17e0:
        break;
    default:
        if (!st.bad_pc) st.bad_pc = pc;
        return false;
    }
    st.calls++;
    ctx.pc = ctx.gpr[0].u32[3] & spu::PC_MASK;
    return true;
}

Executor::Executor(uint32_t n) : spu_num(n), t(std::make_unique<spu::SpuThread>()) {
    t->id = 0x7d000000u | n;
    t->slot = 48 + n;
    t->name = "edge vspu";
    t->job_mode = true;
    t->job_stop = kJobRet;
    t->job_next = no_next;
    t->job_trap = trap;
    t->job_user = &api;
    t->ctx.pc = kJobRet;
}

Executor::~Executor() = default;

namespace {

constexpr uint32_t kCodeBase = 0x4000;
constexpr uint32_t kCodeCheckEnd = 0x10430;
std::atomic<uint64_t> g_code_repairs{0};

uint64_t code_check_every() {
    static const uint64_t n = [] {
        const char* e = std::getenv("GOWA_JOBSYS_EDGE_CODECHECK");
        return e && *e ? std::strtoull(e, nullptr, 10) : 1024ull;
    }();
    return n;
}

}

uint64_t code_repairs() { return g_code_repairs.load(std::memory_order_relaxed); }

void Executor::check_code(const JobRef& j, const Cmd& c, bool ran_spu) {
    uint32_t len = std::min<uint32_t>(c.code_len, kCodeCheckEnd - kCodeBase);
    const uint8_t* g = static_cast<const uint8_t*>(vm::ptr(c.module));
    uint8_t* l = t->ls + kCodeBase;
    if (std::memcmp(l, g, len) == 0) return;
    uint32_t first = 0, last = 0, n = 0;
    for (uint32_t i = 0; i < len; i++)
        if (l[i] != g[i]) {
            if (!n) first = i;
            last = i;
            n++;
        }
    uint64_t k = g_code_repairs.fetch_add(1, std::memory_order_relaxed);
    if (k < 20)
        LOG_ERROR("jsedge", "edge executor %u: resident code differs from the module at LS 0x%05x-0x%05x (%u bytes) after job hdr 0x%x index %u cmd 0x%x%s, reloading it", spu_num, kCodeBase + first, kCodeBase + last, n, j.hdr, j.index, c.ea,
                  ran_spu ? " (SPU code ran in this job)" : "");
    if (k < 4) {
        char name[64];
        std::snprintf(name, sizeof name, "edge_code_%llu.bin", static_cast<unsigned long long>(k));
        if (FILE* f = std::fopen(name, "wb")) {
            std::fwrite(t->ls, 1, spu::LS_SIZE, f);
            std::fclose(f);
        }
    }
    std::memcpy(l, g, len);
    t->mark_ls(kCodeBase, len);
    hcache.assign(spu::LS_SIZE / 4, nullptr);
    hvalid.assign(spu::LS_SIZE / 4, 0);
}

bool Executor::run_edge(const JobRef& j, const Cmd& c, bool reference, Sandbox* pre) {
    spu::SpuThread& th = *t;
    toggle ^= 1;
    bool load_code = code_ea != c.module;
    code_ea = c.module;
    uint64_t exec0 = th.exec_insns;
    uint64_t p0 = g_prof ? __rdtsc() : 0, pd0 = t_dma;
    prepare_ls(th.ls, th.ctx, j, c, spu_num, toggle, load_code, !pm_loaded);
    if (!pm_loaded) {
        th.mark_all_ls();
        pm_loaded = true;
    } else {
        th.mark_ls(0x180, 0x80);
        th.mark_ls(kCmdLs, 0x100);
        th.mark_ls(kApiLs, 0x100);
        if (load_code) th.mark_ls(0x4000, c.code_len);
        th.mark_ls(set_base(c, 1, toggle), c.sets[1].pages * 0x400u);
    }
    api = ApiState{};
    for (int k = 0; k < 4; k++) api.mask[k] = 0xff;
    api.buf = toggle;
    api.cmd = &c;
    th.tag_mask = th.tag_stat = th.atomic_stat = 0;
    th.tag_stat_count = th.atomic_stat_count = false;
    th.raddr = 0;
    th.events = 0;
    th.event_mask = 0;
    th.event_count = false;
    if (pre) {
        if (!shadow) shadow = std::make_unique<spu::SpuThread>();
        spu::SpuThread& s = *shadow;
        std::memcpy(s.ls, th.ls, spu::LS_SIZE);
        std::memcpy(s.ctx.gpr, th.ctx.gpr, sizeof th.ctx.gpr);
        s.ctx.pc = th.ctx.pc;
        s.ctx.interrupts_enabled = th.ctx.interrupts_enabled;
        s.tag_mask = s.tag_stat = s.atomic_stat = 0;
        s.tag_stat_count = s.atomic_stat_count = false;
        s.raddr = 0;
        ApiState st = api;
        if (!run_sandboxed(s, st, *pre)) pre->abort = true;
    }
    FastHost fh(th, hc);
    if (load_code || hvalid.empty()) {
        hcache.assign(spu::LS_SIZE / 4, nullptr);
        hvalid.assign(spu::LS_SIZE / 4, 0);
    }
    if (g_prof) prof_add_impl(load_code ? "prepare_ls+code" : "prepare_ls", __rdtsc() - p0);
    th.stop_flag.store(false);
    Fast r;
    if (reference) {
        r = Fast::Fallback;
    } else if (hle::enabled() && hle::run(*this, fh, j, c)) {
        r = Fast::Done;
    } else {
        if (hle::enabled()) {
            hle_fallbacks++;
            prepare_ls(th.ls, th.ctx, j, c, spu_num, toggle, true, false);
            th.mark_all_ls();
            api = ApiState{};
            for (int k = 0; k < 4; k++) api.mask[k] = 0xff;
            api.buf = toggle;
            api.cmd = &c;
            th.tag_mask = th.tag_stat = th.atomic_stat = 0;
            th.tag_stat_count = th.atomic_stat_count = false;
            th.raddr = 0;
        }
        r = fast_loop(th, fh, api, hcache, hvalid, dispatches);
    }
    if (r == Fast::Fallback) {
        fallbacks++;
        uint64_t x0 = g_prof ? __rdtsc() : 0, xd0 = t_dma;
        th.exec_host = reference ? nullptr : &fh;
        th.exec();
        th.exec_host = nullptr;
        if (g_prof) prof_add_impl("fallback exec", __rdtsc() - x0 - (t_dma - xd0));
    }
    if (g_prof) {
        prof_add_impl("dma", t_dma - pd0);
        prof_add_impl("job total", __rdtsc() - p0);
    }
    fail_pc = th.ctx.pc;
    bool ok = th.ctx.pc == kJobRet && !th.ctx.halted && !api.bad_pc;
    th.ctx.halted = false;
    if (uint64_t every = code_check_every(); every && !reference) {
        bool ran_spu = th.exec_insns != exec0;
        if (ran_spu || ++code_jobs >= every) {
            code_jobs = 0;
            check_code(j, c, ran_spu);
        }
    }
    return ok;
}

void prof_report(uint64_t jobs) {
    if (!g_prof || !jobs) return;
    static const double hz = [] {
        uint64_t q0 = host::qpc(), t0 = __rdtsc();
        host::sleep_ms(20);
        return double(__rdtsc() - t0) * double(host::qpf()) / double(host::qpc() - q0);
    }();
    std::vector<std::pair<uint64_t, std::pair<const char*, uint64_t>>> rows;
    for (ProfEntry& e : g_prof_tab) {
        const char* n = e.name.load();
        if (!n) continue;
        uint64_t t = e.ticks.exchange(0), c = e.calls.exchange(0);
        if (t) rows.push_back({t, {n, c}});
    }
    std::sort(rows.rbegin(), rows.rend());
    std::string line;
    for (size_t i = 0; i < rows.size() && i < 24; i++) {
        char buf[160];
        std::snprintf(buf, sizeof buf, " [%s %.2f us %.1f calls]", rows[i].second.first, double(rows[i].first) / hz * 1e6 / double(jobs), double(rows[i].second.second) / double(jobs));
        line += buf;
    }
    LOG_INFO("jsedge", "5s profile per edge job:%s", line.c_str());
}

bool run_sandboxed(spu::SpuThread& s, ApiState& api, Sandbox& sb) {
    s.job_mode = true;
    s.job_stop = kJobRet;
    s.job_next = no_next;
    s.job_trap = trap;
    s.job_user = &api;
    s.mark_all_ls();
    SandboxHost h(s, sb);
    s.exec_host = &h;
    s.stop_flag.store(false);
    s.ctx.halted = false;
    s.exec();
    s.exec_host = nullptr;
    bool ok = s.ctx.pc == kJobRet && !s.ctx.halted && !api.bad_pc && !sb.abort;
    s.stop_flag.store(false);
    s.ctx.halted = false;
    return ok;
}

}
