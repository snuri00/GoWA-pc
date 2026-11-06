#include "edge_hle.h"

#include "../spu/edge/edge_glue.h"
#include "../spu/edge/spu_float.h"
#include "../spu/hle.h"
#include "../spu/thread.h"

#include "runtime/log.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include <immintrin.h>

namespace jsedge::hle {

namespace {

using edge::glue::rd16;
using edge::glue::rd32;
using edge::glue::wr16;
using edge::glue::wr32;

constexpr uint32_t kSpEntry = 0x3f990;
constexpr uint32_t kSpMain = kSpEntry - 96 - 432;
constexpr uint32_t kSpDriver = kSpMain - 0x2b0;
constexpr uint32_t kSpOut = kSpMain - 464;
constexpr uint32_t kParams = 0x10650;
constexpr uint32_t kCtx = 0x107a0;
constexpr uint32_t kElem = 0x106b0;
constexpr uint32_t kView = 0x10500;
constexpr uint32_t kMatA = 0x10480;
constexpr uint32_t kMatB = 0x104c0;
constexpr uint32_t kJobPtr = 0x10b00;
constexpr uint32_t kFetch16 = 0x10710;

thread_local uint64_t t_exec_ticks = 0;
thread_local uint32_t t_exec_pc = 0;
thread_local uint32_t t_exec_calls = 0;

const uint32_t g_decline = [] {
    const char* e = std::getenv("GOWA_JOBSYS_EDGE_HLE_DECLINE");
    return e && *e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 16)) : 0u;
}();

std::atomic<uint64_t> g_fn_fallbacks[3], g_restarts{0}, g_late_restarts{0};

const bool g_on = [] {
    const char* e = std::getenv("GOWA_JOBSYS_EDGE_HLE");
    return !(e && *e && e[0] == '0');
}();

struct HostMfc final : edge::glue::Mfc {
    spu::Host& h;
    bool committed = false;
    explicit HostMfc(spu::Host& hh) : h(hh) {}
    void cmd(uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag, uint32_t c) {
        h.write_channel(16, lsa);
        h.write_channel(17, 0);
        h.write_channel(18, ea);
        h.write_channel(19, size);
        h.write_channel(20, tag);
        h.write_channel(21, c);
    }
    void get(uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag) override { cmd(lsa, ea, size, tag, 0x40); }
    void put(uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag, bool fence) override {
        committed = true;
        cmd(lsa, ea, size, tag, fence ? 0x22 : 0x20);
    }
    void getllar(uint32_t lsa, uint32_t ea) override {
        cmd(lsa, ea, 128, 0, 0xd0);
        h.read_channel(27);
    }
    bool putllc(uint32_t lsa, uint32_t ea) override {
        committed = true;
        cmd(lsa, ea, 128, 0, 0xb4);
        return h.read_channel(27) == 0;
    }
    void putlluc(uint32_t lsa, uint32_t ea) override {
        committed = true;
        cmd(lsa, ea, 128, 0, 0xb0);
        h.read_channel(27);
    }
    void wait_tag(uint32_t mask) override {
        h.write_channel(22, mask);
        h.write_channel(23, 2);
        h.read_channel(24);
    }
    uint32_t decrementer() override { return h.read_channel(8); }
};

struct Job {
    Executor& x;
    spu::SpuThread& th;
    spu::Context& c;
    uint8_t* ls;
    spu::Host& h;
    HostMfc m;
    ApiState& api;
    uint32_t tag1 = 0;
    uint32_t nv = 0;
    bool failed = false;
    const char* fail_what = nullptr;
    Job(Executor& xx, spu::Host& hh) : x(xx), th(*xx.t), c(xx.t->ctx), ls(xx.t->ls), h(hh), m(hh), api(xx.api) {}
};

void setr(spu::Context& c, int r, uint32_t w0, uint32_t w1 = 0, uint32_t w2 = 0, uint32_t w3 = 0) {
    c.gpr[r].u32[3] = w0;
    c.gpr[r].u32[2] = w1;
    c.gpr[r].u32[1] = w2;
    c.gpr[r].u32[0] = w3;
}

void seti(spu::Context& c, int r, uint32_t v) { setr(c, r, v, v, v, v); }

void setq(spu::Context& c, int r, const uint8_t* ls, uint32_t a) {
    a &= 0x3fff0;
    setr(c, r, rd32(ls, a), rd32(ls, a + 4), rd32(ls, a + 8), rd32(ls, a + 12));
}

uint32_t pw(const spu::Context& c, int r) { return c.gpr[r].u32[3]; }

void set_sp(spu::Context& c, uint32_t sp) { setr(c, 1, sp, 0x1980 - (kSpEntry - sp), 0u - (kSpEntry - sp), 0u - (kSpEntry - sp)); }

void copy_ls(uint8_t* ls, uint32_t dst, uint32_t src, uint32_t n) { std::memmove(ls + (dst & 0x3ffff), ls + (src & 0x3ffff), n); }

void fail(Job& j, const char* what) {
    if (!j.failed) j.fail_what = what;
    j.failed = true;
}

bool run_until_ret(Job& j) {
    spu::Context& c = j.c;
    spu::SpuThread& t = j.th;
    for (uint32_t guard = 0; guard < 1000000; guard++) {
        uint32_t pc = c.pc & spu::PC_MASK;
        c.pc = pc;
        if (pc == kJobRet) return true;
        if (pc < 0x4000) {
            if (!api_call(c, j.api)) return false;
            continue;
        }
        const spu::hle::Function* f = nullptr;
        if (pc < 0x10430) {
            uint32_t k = pc >> 2;
            if (!j.x.hvalid[k]) {
                j.x.hcache[k] = spu::hle::any_at(pc) ? spu::hle::lookup(t.ls, pc) : nullptr;
                j.x.hvalid[k] = 1;
            }
            f = j.x.hcache[k];
        } else {
            if (spu::hle::any_at(pc)) f = spu::hle::lookup(t.ls, pc);
            if (!f && spu::hle::dyn_at(pc)) f = spu::hle::lookup_dyn(t.ls, pc);
        }
        if (f && !f->host_handler) {
            if (f->handler(c)) {
                c.pc = c.gpr[0].u32[3] & spu::PC_MASK;
                continue;
            }
            f = spu::hle::lookup_next(t.ls, pc, f);
            if (f && !f->host_handler) f = nullptr;
        }
        if (f) {
            c.executed = 0;
            if (f->host_handler(c, j.h)) {
                if (c.halted) return false;
                c.irq_check = false;
                continue;
            }
            c.pc = pc;
        }
        if (!t_exec_pc) t_exec_pc = pc | 0x80000000u;
        t.exec_host = &j.h;
        t.stop_flag.store(false);
        uint64_t x0 = __rdtsc();
        t.exec();
        t_exec_ticks += __rdtsc() - x0;
        t_exec_calls++;
        t.exec_host = nullptr;
        if (c.halted) return false;
    }
    return false;
}

const char* pc_name(uint32_t pc) {
    switch (pc) {
    case 0x88b0: return "hle:init_segment";
    case 0xdbc8: return "hle:decompress";
    case 0xdcb0: return "hle:decompress_indexes";
    case 0xe550: return "hle:skin";
    case 0xf538: return "hle:transform";
    case 0xbbc8: return "hle:normalize";
    case 0xd618: return "hle:cull";
    case 0xa880: return "hle:binormals";
    case 0x73e0: return "hle:soft_body";
    case 0xd158: return "hle:compress";
    default: return "hle:overlay";
    }
}

bool call_fn(Job& j, uint32_t pc, uint32_t sp) {
    setr(j.c, 0, kJobRet);
    set_sp(j.c, sp);
    j.c.pc = pc;
    j.c.interrupts_enabled = true;
    bool prof = prof_on();
    uint64_t t0 = prof ? __rdtsc() : 0;
    uint64_t e0 = t_exec_ticks;
    uint32_t c0 = t_exec_calls;
    uint32_t args[12];
    for (int r = 0; r < 12; r++) args[r] = pw(j.c, 3 + r);
    uint32_t w0 = rd32(j.ls, 0x400c);
    t_exec_pc = 0;
    bool ok = run_until_ret(j);
    if (rd32(j.ls, 0x400c) != w0) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 20)
            LOG_ERROR("jsedge", "edge code at LS 0x400c overwritten (0x%08x -> 0x%08x) during call 0x%05x%s0x%05x, args r3-r14 %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x sp 0x%05x", w0, rd32(j.ls, 0x400c), pc,
                      t_exec_pc ? ", SPU code entered at " : ", no SPU code, last pc ", t_exec_pc ? t_exec_pc & 0x3ffff : j.c.pc, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], sp);
    }
    if (prof) {
        prof_add(pc_name(pc), __rdtsc() - t0);
        if (t_exec_calls != c0) prof_add("hle:exec-fallback", t_exec_ticks - e0);
    }
    if (ok) return true;
    fail(j, "call");
    return false;
}

void api_get_buffer(Job& j, uint32_t set, uint32_t alloc, spu::v128& out) {
    setr(j.c, 0, kJobRet);
    seti(j.c, 3, 0);
    seti(j.c, 4, set);
    seti(j.c, 5, 0);
    seti(j.c, 6, alloc);
    j.c.pc = 0x2fd8;
    if (!api_call(j.c, j.api)) fail(j, "GetBuffer");
    out = j.c.gpr[3];
}

spu::v128 api_get_tag(Job& j) {
    setr(j.c, 0, kJobRet);
    j.c.pc = 0x17b8;
    if (!api_call(j.c, j.api)) fail(j, "GetTag");
    return j.c.gpr[3];
}

void api_release_tag(Job& j, const spu::v128& tags) {
    setr(j.c, 0, kJobRet);
    j.c.gpr[3] = tags;
    j.c.pc = 0x17c0;
    if (!api_call(j.c, j.api)) fail(j, "ReleaseTag");
}

void api_load_next(Job& j) {
    j.m.committed = true;
    setr(j.c, 0, kJobRet);
    j.c.pc = 0x17e0;
    if (!api_call(j.c, j.api)) fail(j, "LoadNextJob");
}

uint32_t buf_base(const spu::v128& q) { return ((q.u32[2] >> 24) & 0xff) * 0x400u; }
uint32_t buf_size(const spu::v128& q) { return ((q.u32[2] >> 8) & 0xff) * 0x400u; }

uint32_t setup_9990(Job& j, uint32_t dst, const spu::v128& q, uint32_t count) {
    uint32_t base = buf_base(q), size = buf_size(q);
    uint32_t bytes = count * 8;
    if (bytes > size) fail(j, "9990 list size");
    uint32_t list = base + size - bytes;
    uint32_t cursor = base;
    for (uint32_t k = 0; k < count; k++) {
        uint32_t sz = rd32(j.ls, list + 8 * k) & 0xffff;
        wr32(j.ls, dst + 4 * k, sz ? cursor : 0);
        cursor += sz;
    }
    return cursor;
}

void transpose_to(uint8_t* ls, uint32_t dst, uint32_t src) {
    uint32_t w[4][4];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) w[r][c] = rd32(ls, src + 16 * r + 4 * c);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) wr32(ls, dst + 16 * r + 4 * c, w[c][r]);
}

void identity_to(uint8_t* ls, uint32_t dst) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) wr32(ls, dst + 16 * r + 4 * c, r == c ? 0x3f800000u : 0u);
}

void spin_decrementer(Job& j, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) j.m.decrementer();
}

void fetch16(Job& j, uint32_t ea, uint32_t tag) {
    edge::glue::get_chunked(j.m, kFetch16, ea, 0x10, tag);
}

bool driver_51d0(Job& j);
void output_9080(Job& j, bool empty, uint32_t r8);

void main_body(Job& j) {
    uint8_t* ls = j.ls;
    spu::Context& c = j.c;
    uint32_t params_src = rd32(ls, kApiLs + 0x20);
    copy_ls(ls, kParams, params_src, 0x60);
    spu::v128 q1;
    api_get_buffer(j, 1, 1, q1);
    uint32_t r84 = setup_9990(j, kElem, q1, 22);
    api_get_buffer(j, 1, 1, q1);
    j.tag1 = q1.u32[3] & 0xffff;
    spu::v128 q2;
    api_get_buffer(j, 2, 0, q2);
    uint32_t r82 = 0;
    spu::v128 tagq{};
    if (rd32(ls, kApiLs + 0xb0) != 0) {
        tagq = api_get_tag(j);
        r82 = tagq.u32[3];
    } else {
        uint32_t st = edge::glue::ring_stall_check(ls, j.m, kParams, j.tag1, (kSpMain + 223) & ~127u, kSpMain);
        if (st != 0) spin_decrementer(j, 1600);
        else
            for (int k = 0; k < 4; k++) wr32(ls, kApiLs + 0xa0 + 4 * k, 1);
        for (int k = 0; k < 4; k++) wr32(ls, 0x10b10 + 4 * k, 1);
        api_load_next(j);
        return;
    }
    uint32_t mat_ea = rd32(ls, kParams + 0x18);
    if (mat_ea) {
        if ((mat_ea & 15) || r82 > 31) fail(j, "fetch16 align");
        fetch16(j, mat_ea, r82);
    }
    uint32_t ma = rd32(ls, kParams + 0xa8);
    if (ma) transpose_to(ls, kMatA, ma);
    else identity_to(ls, kMatA);
    uint32_t mb = rd32(ls, kParams + 0xac);
    if (mb) transpose_to(ls, kMatB, mb);
    else identity_to(ls, kMatB);
    uint32_t view = rd32(ls, kParams + 0xa4);
    copy_ls(ls, kView, view, 0x110);
    uint32_t no_cull = ls[((view + 112) & 0x3fff0) + 1];
    if (no_cull != 0) {
        wr32(ls, kParams + 12, rd32(ls, kParams + 12) | 0x800);
    } else {
        copy_ls(ls, 0x10720, view, 16);
        copy_ls(ls, 0x10770, view + 80, 16);
        copy_ls(ls, 0x10780, view + 96, 16);
        ls[0x10790] = ls[(view + 112) & 0x3ffff];
        uint32_t nplanes = (ls[kParams + 0x49] >> 4) & 15;
        for (uint32_t k = 0; k < nplanes; k++) transpose_to(ls, 0x10610 + 64 * k, view + 144 + 64 * k);
        copy_ls(ls, 0x10730, 0x10610, 64);
    }
    uint32_t cfg = rd32(ls, kParams + 0x94);
    uint32_t nv = rd16(ls, cfg + 8);
    j.nv = nv;
    wr16(ls, cfg + 8, uint16_t((nv + 7) & ~7u));
    ls[(cfg + 1) & 0x3ffff] = ls[kParams + 0x4a];
    uint32_t flags = rd32(ls, kParams + 12) & 0xffffff;
    setr(c, 3, kCtx, kCtx, kCtx, kCtx);
    seti(c, 4, cfg);
    seti(c, 5, rd32(ls, kParams + 0x98));
    setq(c, 6, ls, kParams + 0xa0);
    seti(c, 7, 0x10670);
    seti(c, 8, j.tag1);
    seti(c, 9, buf_base(q1));
    seti(c, 10, buf_size(q1));
    seti(c, 11, buf_base(q2));
    seti(c, 12, buf_size(q2));
    seti(c, 13, flags);
    seti(c, 14, r84);
    seti(c, 15, 0xf7d0);
    seti(c, 16, 0x10720);
    seti(c, 17, kParams);
    seti(c, 18, rd32(ls, kElem + 8));
    seti(c, 19, rd32(ls, kElem + 4));
    if (!call_fn(j, 0x88b0, kSpMain)) return;
    if (mat_ea) {
        j.m.wait_tag(1u << r82);
        fetch16(j, mat_ea, r82);
        j.m.wait_tag(1u << r82);
    }
    api_release_tag(j, tagq);
    wr32(ls, kSpMain + 48 + 8, 0);
    bool skip = false;
    if (mat_ea) skip = rd32(ls, kFetch16 + 8) == 0 && rd32(ls, kFetch16 + 12) == 0;
    uint32_t r8 = rd32(ls, kElem + 12);
    if (skip) {
        uint32_t st = edge::glue::ring_stall_check(ls, j.m, kParams, rd32(ls, 0x108f4), (kSpMain + 223) & ~127u, kSpMain);
        if (st != 0)
            for (int k = 0; k < 4; k++) wr32(ls, kApiLs + 0xa0 + 4 * k, 0);
        for (int k = 0; k < 4; k++) wr32(ls, 0x10b10 + 4 * k, 1);
        api_load_next(j);
        uint32_t r = edge::glue::ringb_reserve(ls, j.m, kCtx, kSpMain + 32, kParams);
        if (r != 0) edge::glue::ringb_commit(ls, j.m, kCtx, kSpMain + 32, kParams, kSpMain);
        j.m.wait_tag(1u << rd32(ls, 0x108f0));
        edge::glue::set_ctx_scratch_ptr(ls, kCtx, rd32(ls, kSpMain + 64 + 12));
        output_9080(j, true, r8);
        return;
    }
    if (!driver_51d0(j)) return;
    output_9080(j, false, r8);
}

struct Drv {
    uint32_t r[128] = {};
};

void load_saved(spu::Context& c, const Drv& d) {
    for (int r = 80; r <= 100; r++) seti(c, r, d.r[r]);
    seti(c, 126, 0x4000);
}

bool decompress_dbf8(Job& j, const Drv& d, uint32_t ctx, uint32_t stream, uint32_t desc, uint32_t second, uint32_t r7) {
    uint8_t* ls = j.ls;
    spu::Context& c = j.c;
    uint32_t sp = kSpDriver - 96;
    wr32(ls, ctx + 84, stream);
    Drv w = d;
    w.r[80] = r7;
    w.r[81] = ctx;
    w.r[82] = second;
    w.r[83] = stream;
    load_saved(c, w);
    seti(c, 3, ctx);
    seti(c, 4, stream);
    seti(c, 5, desc);
    seti(c, 6, (uint32_t(ls[(ctx + 15) & 0x3ffff]) << 24) | (uint32_t(ls[ctx & 0x3ffff]) << 16) | (uint32_t(ls[(ctx + 1) & 0x3ffff]) << 8) | ls[(ctx + 2) & 0x3ffff]);
    seti(c, 7, rd32(ls, ctx + 108));
    seti(c, 8, rd32(ls, ctx + 124));
    if (!call_fn(j, 0xdbc8, sp)) return false;
    if (second) {
        load_saved(c, w);
        seti(c, 3, ctx);
        seti(c, 4, second);
        seti(c, 5, r7);
        seti(c, 6, rd32(ls, ctx));
        seti(c, 7, rd32(ls, ctx + 124));
        seti(c, 8, rd32(ls, ctx + 128));
        if (!call_fn(j, 0xdbc8, sp)) return false;
    }
    uint32_t r4 = second > stream ? second : stream;
    edge::glue::set_ctx_scratch_ptr(ls, ctx, r4);
    return true;
}

bool indexes_e4d0(Job& j, const Drv& d, uint32_t ctx, uint32_t indexes) {
    uint8_t* ls = j.ls;
    spu::Context& c = j.c;
    if (rd16(ls, ctx + 400) == 0) return true;
    uint32_t flavor = (ls[(ctx + 6) & 0x3ffff] >> 4) & 15;
    if (flavor - 2 <= 1) {
        Drv w = d;
        w.r[80] = ctx;
        w.r[81] = indexes;
        load_saved(c, w);
        seti(c, 3, ctx);
        seti(c, 4, indexes);
        if (!call_fn(j, 0xdcb0, kSpDriver - 64)) return false;
    }
    wr32(ls, ctx + 80, indexes);
    return true;
}

bool native_call(Job& j, const Drv& d, uint32_t pc, uint32_t sp, uint32_t r3, uint32_t r4, uint32_t r5 = 0, uint32_t r6 = 0) {
    load_saved(j.c, d);
    seti(j.c, 3, r3);
    seti(j.c, 4, r4);
    seti(j.c, 5, r5);
    seti(j.c, 6, r6);
    return call_fn(j, pc, sp);
}

bool rigid_5838(Job& j, Drv& d, uint32_t r2) {
    uint8_t* ls = j.ls;
    spu::Context& c = j.c;
    const uint32_t ctx = kCtx;
    const uint32_t sp = kSpDriver;
    d.r[90] = 0x80000000u;
    if (!native_call(j, d, 0xf538, sp, d.r[89], d.r[80], d.r[80], d.r[82])) return false;
    spuf::CsrGuard g;
    const uint32_t one = 0x3f800000u;
    uint32_t m[3][4];
    for (int r = 0; r < 3; r++)
        for (int i = 0; i < 4; i++) m[r][i] = rd32(ls, d.r[82] + 16 * r + 4 * i);
    uint32_t r50 = (d.r[86] & 0x80000u) ? 0xbf800000u : one;
    wr32(ls, sp + 80, 0);
    wr32(ls, sp + 84, 0);
    wr32(ls, sp + 88, 0);
    wr32(ls, sp + 92, r50);
    float len2[3];
    for (int r = 0; r < 3; r++) {
        float sq[4];
        for (int i = 0; i < 4; i++) sq[i] = spuf::fm(spuf::fl(m[r][i]), spuf::fl(m[r][i]));
        len2[r] = spuf::fa(spuf::fa(sq[0], sq[1]), sq[2]);
    }
    float inv[3];
    for (int r = 0; r < 3; r++) {
        float e = spuf::frest(len2[r]);
        float t = spuf::fnms(e, len2[r], spuf::fl(one));
        inv[r] = spuf::fma(t, e, e);
    }
    uint32_t nrow[3][4];
    for (int r = 0; r < 3; r++) {
        for (int i = 0; i < 4; i++) nrow[r][i] = spuf::bits(spuf::fma(inv[r], spuf::fl(m[r][i]), 0.0f));
        nrow[r][3] = 0;
    }
    for (int r = 0; r < 3; r++)
        for (int i = 0; i < 4; i++) wr32(ls, sp + 32 + 16 * r + 4 * i, nrow[r][i]);
    float sum = 0;
    for (int r = 0; r < 3; r++) {
        float sq[4];
        for (int i = 0; i < 4; i++) sq[i] = spuf::fm(spuf::fl(nrow[r][i]), spuf::fl(nrow[r][i]));
        float l = spuf::fa(spuf::fa(sq[0], sq[1]), sq[2]);
        float dlt = spuf::fs(l, spuf::fl(one));
        float ad = spuf::fl(spuf::bits(dlt) & 0x7fffffffu);
        sum = r == 0 ? ad : spuf::fa(sum, ad);
    }
    uint32_t r84 = spuf::fcgt(sum, spuf::fl(0x3c23d70au)) ? 1 : 0;
    d.r[84] = r84;
    uint32_t t3 = edge::glue::ctx_attr3_table(ls, ctx);
    d.r[80] = t3;
    if (t3 != 0) {
        if (!native_call(j, d, 0xf538, sp, d.r[89], t3, t3, sp + 32)) return false;
        uint32_t t4 = edge::glue::ctx_attr4_table(ls, ctx);
        d.r[82] = t4;
        d.r[90] = r84 == 0 ? 0xff : 0;
        if (!native_call(j, d, 0xf538, sp, d.r[89], t4, t4, sp + 32)) return false;
        if (r84 != 0) {
            if (!native_call(j, d, 0xbbc8, sp, t3, d.r[89])) return false;
            if (!native_call(j, d, 0xbbc8, sp, t4, d.r[89])) return false;
        }
    }
    uint32_t t2 = edge::glue::ctx_attr2_table(ls, ctx);
    d.r[80] = t2;
    if (t2 != 0) {
        d.r[91] = r84 == 0 ? 0xff : 0;
        d.r[82] = r84 == 0 ? 0xffffffffu : 0;
        if (!native_call(j, d, 0xf538, sp, d.r[89], t2, t2, sp + 32)) return false;
        if (r84 != 0)
            if (!native_call(j, d, 0xbbc8, sp, t2, d.r[89])) return false;
    }
    (void)r2;
    (void)c;
    return true;
}

bool driver_51d0(Job& j) {
    uint8_t* ls = j.ls;
    spu::Context& c = j.c;
    Drv d;
    for (int k = 80; k <= 100; k++) d.r[k] = pw(c, k);
    uint32_t* r = d.r;
    const uint32_t ctx = kCtx;
    const uint32_t sp = kSpDriver;
    const uint32_t stackbuf = (sp + 223) & ~127u;
    r[81] = ctx;
    r[82] = kMatA;
    r[83] = kParams;
    r[85] = kSpMain + 32;
    r[88] = kMatB;
    r[89] = j.nv;
    r[90] = rd32(ls, kParams + 0x78);
    r[91] = rd32(ls, kParams + 0x88);
    r[92] = rd32(ls, kElem);
    r[94] = rd32(ls, kParams + 0x74);
    r[96] = ls[kParams + 0x48];
    r[97] = rd32(ls, kParams + 0x70);
    r[99] = rd32(ls, kParams + 8);
    r[100] = (uint32_t(ls[kParams + 9]) << 24) | (uint32_t(ls[kParams + 10]) << 16) | (uint32_t(ls[kParams + 11]) << 8) | ls[kParams + 12];
    r[98] = rd32(ls, kParams + 56);
    r[86] = rd32(ls, kParams + 12) & 0xffffff;
    r[84] = (uint32_t(ls[kParams + 6]) << 24) | (uint32_t(ls[kParams + 7]) << 16) | (uint32_t(ls[kParams + 8]) << 8) | ls[kParams + 9];
    r[80] = r[84] & 0xffff;
    if (!decompress_dbf8(j, d, ctx, rd32(ls, kParams + 0x8c), 0x10980, 0, 0)) return false;
    uint32_t r3 = 0, r5 = 0;
    r[80] = ls[(ctx + 6) & 0x3ffff] & 15;
    uint32_t r2 = edge::glue::attr_map_find(ls, rd32(ls, ctx + 116), 0x1f) != -1 ? 1 : 0;
    if (r[80] == 0) goto L5820;
    r[87] = r2 == 0 ? 0xff : 0;
    r[82] = 0xffff;
    r[80] = r[84] & 0xffff;
    if (r2 != 0) goto L5698;
L5328:
    if (!native_call(j, d, 0xe550, sp, ctx, r[90], r[80], r[91])) return false;
    r[95] = r[87] ? 0xffffffffu : 0;
    if (r[87] == 0) goto L5ae0;
L5348:
    r3 = edge::glue::ring_stall_check(ls, j.m, kParams, rd32(ls, ctx + 340), stackbuf, sp);
    if (r3 != 0)
        for (int k = 0; k < 4; k++) wr32(ls, kApiLs + 0xa0 + 4 * k, 0);
    for (int k = 0; k < 4; k++) wr32(ls, 0x10b10 + 4 * k, 1);
    api_load_next(j);
    if (!indexes_e4d0(j, d, ctx, r[94])) return false;
    r3 = edge::glue::ringb_reserve(ls, j.m, ctx, r[85], kParams);
    if (r3 != 0) edge::glue::ringb_commit(ls, j.m, ctx, r[85], kParams, sp);
    j.m.wait_tag(1u << rd32(ls, ctx + 336));
    edge::glue::set_ctx_scratch_ptr(ls, ctx, rd32(ls, r[85] + 44));
    r[85] = r[86] & 0x800;
    if (r[85] != 0) goto L548c;
    {
        uint32_t flavor = (((r[86] >> 19) ^ (r[86] >> 20)) & 1) ? 3 : 2;
        r[93] = rd32(ls, kParams + 70) & ~15u;
        bool force = (r[93] & 0xff) == 0x10;
        r[83] = force ? 0xffffffffu : 0;
        if (!force) {
            r[84] = r[86] & 0x1000;
            if (r[84] != 0) return true;
        }
        if (!native_call(j, d, 0xd618, sp, ctx, flavor, 0, 0)) return false;
        if (pw(c, 3) == 0) return true;
    }
L548c:
    r[80] = (r[86] >> 2) & 1;
    if (r[80] != 0) goto L5af0;
L54a0:
    r3 = edge::glue::slot_table(ls, ctx, 0x14);
L54b0:
    {
        r[87] = r3;
        uint32_t cond = ((r[86] >> 22) | ((r[96] != 1 ? 1u : 0u) & (r3 != 0 ? 1u : 0u))) & 1;
        r[96] = 0u - cond;
        r[82] = cond;
        if (!native_call(j, d, 0xa880, sp, ctx, r[89])) return false;
        r3 = pw(c, 3);
        r[85] = r3;
        if (r3 == 0) goto L5630;
    }
L5500:
    r[83] = r[80] == 0 ? 0xff : 0;
    r[84] = r[82] == 0 ? 0xff : 0;
    if (r[80] == 0) goto L5634;
    if ((r[86] & 0x400) == 0) goto L5650;
L5520:
    r[97] = r[83] ? 0xffffffffu : 0;
    r[80] = 0;
    r[91] = 0;
    r[90] = 0;
    r[88] = 0;
    if (r[83] == 0) goto L5b88;
L5538:
    r[82] = edge::glue::ctx_position_table(ls, ctx);
    r[100] = r[84] ? 0xffffffffu : 0;
    r[99] = r[100] | (r[83] ? 0 : 0xffffffffu);
    if (r[99] == 0) goto L5c10;
L5570:
    {
        uint32_t r70 = r[98] & 255;
        spuf::CsrGuard g;
        uint32_t fbits = spuf::cuflt(r70, 155);
        uint32_t r81b = spuf::bits(spuf::fm(spuf::fl(fbits), spuf::fl(0x3b808080u)));
        load_saved(c, d);
        seti(c, 3, ctx);
        seti(c, 4, r[80]);
        seti(c, 5, r[91]);
        seti(c, 6, r[90]);
        seti(c, 7, r[88]);
        seti(c, 8, r[87]);
        seti(c, 9, r[82]);
        seti(c, 10, r[85]);
        seti(c, 11, r[89]);
        seti(c, 12, r[92]);
        seti(c, 13, r[86]);
        seti(c, 14, r81b);
        seti(c, 81, r81b);
        seti(c, 98, fbits);
        seti(c, 89, 0x10203);
        seti(c, 70, r70);
        seti(c, 69, 0x3b808080u);
        return call_fn(j, 0x73e0, sp);
    }
L5630:
    r[80] = 0;
L5634:
    r[84] = r[82] == 0 ? 0xff : 0;
    r[83] = r[80] == 0 ? 0xff : 0;
    if (r[84] != 0) return true;
    if ((r[86] & 0x400) != 0) goto L5520;
L5650:
    if (r[97] == 0) goto L5520;
    {
        uint32_t r7 = (r[86] >> 18) & 1;
        r3 = r[92];
        r[92] = 0xffff;
        r5 = r[97];
        uint32_t r6 = r[100] & 255;
        r[80] = 0;
        uint32_t r4 = r[92] & r[99];
        r[91] = 0;
        uint32_t res = 0;
        if ((g_decline & 1) || !edge::glue::softprep_9a60(ls, j.m, r3, r4, r5, r6, r7, sp, res)) {
            load_saved(c, d);
            seti(c, 3, r3);
            seti(c, 4, r4);
            seti(c, 5, r5);
            seti(c, 6, r6);
            seti(c, 7, r7);
            if (!call_fn(j, 0x9a60, sp)) return false;
            res = pw(c, 3);
            g_fn_fallbacks[0].fetch_add(1, std::memory_order_relaxed);
        }
        r[97] = r[83] ? 0xffffffffu : 0;
        r[90] = 0;
        r[92] = res;
        r[88] = 0;
        if (r[83] != 0) goto L5538;
        goto L5b88;
    }
L5698:
    r3 = edge::glue::slot_alloc(ls, ctx, 0x1f);
    {
        uint32_t r13 = edge::glue::ctx_uniform_table(ls, ctx, r3);
        uint32_t old96 = rd32(ls, ctx + 96), old100 = rd32(ls, ctx + 100), old104 = rd32(ls, ctx + 104), old92 = rd32(ls, ctx + 92);
        r[95] = old96;
        r[93] = old100;
        r[84] = old92;
        r[88] = old104;
        wr32(ls, ctx + 92, r13);
        wr32(ls, ctx + 96, 0);
        wr32(ls, ctx + 100, 0);
        wr32(ls, ctx + 104, 0);
        uint32_t nv8 = rd16(ls, ctx + 8);
        if (nv8 != 0) {
            uint32_t src = old92, dst = r13, n = 0;
            do {
                n += 8;
                copy_ls(ls, dst, src, 128);
                src += 128;
                dst += 128;
            } while (int32_t(r[82] & rd16(ls, ctx + 8)) > int32_t(n));
        }
        uint32_t fmt = ls[(ctx + 7) & 0x3ffff];
        uint32_t stride = fmt != 0 ? 0x40 : 0x30;
        uint32_t r4 = ((stride & 0xffff) * (r[80] & 0xffff)) + r[90];
        if (!native_call(j, d, 0xe550, sp, ctx, r4, r[80], r[91])) return false;
        wr32(ls, ctx + 92, r[84]);
        wr32(ls, ctx + 96, r[95]);
        wr32(ls, ctx + 100, r[93]);
        wr32(ls, ctx + 104, r[88]);
        goto L5328;
    }
L5820:
    r[87] = r2 == 0 ? 0xff : 0;
    r[80] = edge::glue::ctx_position_table(ls, ctx);
    if (r[87] == 0) goto L5c50;
L5838:
    if (!rigid_5838(j, d, r2)) return false;
    r[95] = r[87] ? 0xffffffffu : 0;
    if (r[87] == 0) goto L5ae0;
    goto L5348;
L5ae0:
    if (!native_call(j, d, 0xa880, sp, ctx, r[89])) return false;
    goto L5348;
L5af0:
    {
        uint32_t m = rd32(ls, ctx + 116);
        int32_t a1b = edge::glue::attr_map_find(ls, m, 0x1b);
        int32_t a1c = edge::glue::attr_map_find(ls, m, 0x1c);
        r[91] = uint32_t(a1b);
        r[82] = (a1b != -1 && a1c != -1) ? 1 : 0;
        int32_t a1d = edge::glue::attr_map_find(ls, m, 0x1d);
        r[90] = r[82] & (a1d != -1 ? 0xffffffffu : 0);
        int32_t a1e = edge::glue::attr_map_find(ls, m, 0x1e);
        r[80] = r[90] & (a1e != -1 ? 0xffffffffu : 0);
        goto L54a0;
    }
L5b88:
    edge::glue::slot_alloc(ls, ctx, 0x1b);
    edge::glue::slot_alloc(ls, ctx, 0x1c);
    edge::glue::slot_alloc(ls, ctx, 0x1d);
    edge::glue::slot_alloc(ls, ctx, 0x1e);
    r[80] = edge::glue::slot_table(ls, ctx, 0x1b);
    r[91] = edge::glue::slot_table(ls, ctx, 0x1c);
    r[90] = edge::glue::slot_table(ls, ctx, 0x1d);
    r[88] = edge::glue::slot_table(ls, ctx, 0x1e);
    goto L5538;
L5c10:
    edge::glue::slot_alloc(ls, ctx, 0x1b);
    r[80] = edge::glue::slot_table(ls, ctx, 0x1b);
    if (r[85] != 0) goto L5570;
    edge::glue::slot_alloc(ls, ctx, 0x1e);
    r[85] = edge::glue::slot_table(ls, ctx, 0x1e);
    goto L5570;
L5c50:
    {
        uint32_t slot = edge::glue::slot_alloc(ls, ctx, 0x1f);
        uint32_t table = edge::glue::ctx_uniform_table(ls, ctx, slot);
        if (!native_call(j, d, 0xf538, sp, r[89], r[80], table, r[88])) return false;
        goto L5838;
    }
}

struct OverlayUser {
    Job* j;
};

bool overlay_call(void* user, uint8_t*, uint32_t entry, const uint32_t* regs) {
    Job& j = *static_cast<OverlayUser*>(user)->j;
    spu::Context& c = j.c;
    for (int i = 0; i < 8; i++) seti(c, 3 + i, regs[i]);
    seti(c, 2, 0x1d70);
    seti(c, 79, 0x4000);
    return call_fn(j, entry, kSpOut);
}

bool hole_grow(Job& j, uint32_t state, uint32_t words) {
    (void)state;
    (void)words;
    fail(j, "hole overflow");
    return false;
}

void output_9080(Job& j, bool empty, uint32_t r8) {
    uint8_t* ls = j.ls;
    spu::Context& c = j.c;
    const uint32_t ctx = kCtx;
    const uint32_t sp = kSpOut;
    const uint32_t inst = kSpMain + 32;
    const uint32_t hole_ea = rd32(ls, kParams + 4);
    if (!empty) {
        uint32_t flags = rd32(ls, kParams + 12);
        if ((flags & 0x800) == 0 && rd16(ls, ctx + 400) == 0) empty = true;
    }
    if (empty) {
        uint32_t state = sp + 48;
        edge::glue::begin_command_buffer_hole(ls, j.m, ctx, state, hole_ea, 0, 0);
        if (!edge::glue::set_instance_state(ls, inst, state)) {
            fail(j, "84c0");
            return;
        }
        edge::glue::end_command_buffer_hole(ls, j.m, ctx, state, hole_ea, 0, 0);
        return;
    }
    uint32_t ring = edge::glue::spu_ring_index(ls);
    uint32_t ni = (rd32(ls, kParams + 12) & 0x800) ? rd16(ls, ctx + 10) : rd16(ls, ctx + 400);
    uint32_t size = 0;
    if ((g_decline & 2) || !edge::glue::output_sizes(ls, ctx, ni, size)) {
        seti(c, 3, ctx);
        seti(c, 4, ni);
        if (!call_fn(j, 0xbb38, sp)) return;
        size = pw(c, 3);
        g_fn_fallbacks[1].fetch_add(1, std::memory_order_relaxed);
    }
    const uint32_t alloc = sp + 112;
    edge::glue::allocate_output_space(ls, j.m, ctx, rd32(ls, kParams), size, alloc, ring, sp);
    edge::glue::output_indexes(ls, j.m, ctx, ni, alloc, sp + 96);
    {
        Drv d;
        for (int k = 80; k <= 100; k++) d.r[k] = pw(c, k);
        d.r[80] = sp + 48;
        d.r[81] = inst;
        d.r[83] = r8;
        d.r[84] = kParams;
        d.r[89] = ni;
        d.r[90] = ctx;
        d.r[95] = alloc;
        load_saved(c, d);
        seti(c, 3, ctx);
        if (!call_fn(j, 0xd158, sp)) return;
    }
    if ((g_decline & 4) || !edge::glue::output_vertexes(ls, j.m, ctx, alloc, sp + 32)) {
        seti(c, 3, ctx);
        seti(c, 4, alloc);
        seti(c, 5, sp + 32);
        if (!call_fn(j, 0xc490, sp)) return;
        g_fn_fallbacks[2].fetch_add(1, std::memory_order_relaxed);
    }
    const uint32_t state = sp + 64;
    edge::glue::begin_command_buffer_hole(ls, j.m, ctx, state, hole_ea, alloc, 1);
    OverlayUser ou{&j};
    edge::glue::OverlayHook hook{overlay_call, &ou};
    if (!edge::glue::set_vertex_data_arrays(ls, ctx, state, rd32(ls, sp + 40), rd32(ls, sp + 36), r8, hook)) {
        fail(j, "5d68");
        return;
    }
    if (!edge::glue::set_instance_state(ls, inst, state)) {
        fail(j, "84c0");
        return;
    }
    uint32_t npass = ls[kParams + 73] >> 4;
    uint32_t mask = ls[kParams + 73] & 15;
    uint32_t io_index = rd32(ls, sp + 100);
    uint32_t p64 = rd16(ls, kParams + 64), p66 = rd16(ls, kParams + 66);
    for (uint32_t k = 0; k < npass; k++) {
        if ((mask & (1u << k)) == 0) continue;
        uint32_t fk = spuf::csflt(int32_t(k), 155);
        for (int i = 0; i < 4; i++) wr32(ls, sp + 80 + 4 * i, fk);
        uint32_t hw = rd16(ls, kParams + 0x44 + 2 * k);
        if (!edge::glue::set_viewport(ls, state, 0, hw, p64, p66, 0, 0x3f800000u)) {
            fail(j, "b3d0");
            return;
        }
        if (!edge::glue::set_scissor(ls, state, 0, hw, p64, p66)) {
            fail(j, "b2d0");
            return;
        }
        if (!edge::glue::set_transform_constant(ls, state, 0, sp + 80)) {
            fail(j, "b1a8");
            return;
        }
        uint32_t cur = rd32(ls, state + 8), end = rd32(ls, state + 4);
        if (cur + 28 > end) {
            if (!hole_grow(j, state, 7)) return;
            cur = rd32(ls, state + 8);
            end = rd32(ls, state + 4);
        }
        uint32_t io = io_index & 0x1fffffffu;
        uint32_t skip = (((io + 127) & ~127u) - io) >> 1;
        wr32(ls, cur, 0x00041714u);
        wr32(ls, cur + 4, 0);
        wr32(ls, cur + 8, 0x0008181cu);
        wr32(ls, cur + 12, io);
        wr32(ls, cur + 16, 0x11);
        wr32(ls, cur + 20, 0x00041808u);
        wr32(ls, cur + 24, 5);
        cur += 28;
        wr32(ls, state + 8, cur);
        uint32_t start = 0, rem = 0;
        if (skip != 0 && ni > skip) {
            if (cur + 8 > end) {
                if (!hole_grow(j, state, 2)) return;
                cur = rd32(ls, state + 8);
                end = rd32(ls, state + 4);
            }
            wr32(ls, cur, 0x00041824u);
            wr32(ls, cur + 4, (skip - 1) << 24);
            cur += 8;
            wr32(ls, state + 8, cur);
            start = skip;
            rem = ni - skip;
        } else {
            rem = ni;
            start = 0;
        }
        while (rem > 0x7ff00u) {
            if (cur + 0x2000 > end) {
                if (!hole_grow(j, state, 0x800)) return;
                cur = rd32(ls, state + 8);
                end = rd32(ls, state + 4);
            }
            wr32(ls, cur, 0x5ffc1824u);
            cur += 4;
            wr32(ls, state + 8, cur);
            rem -= 0x7ff00u;
            uint32_t s = start;
            for (uint32_t n = 0x7ff; n; n--) {
                wr32(ls, cur, s | 0xff000000u);
                s += 256;
                cur += 4;
            }
            wr32(ls, state + 8, cur);
            start += 0x7ff00u;
        }
        uint32_t batches = (rem + 255) >> 8;
        if (cur + batches * 4 + 4 > end) {
            if (!hole_grow(j, state, batches + 1)) return;
            cur = rd32(ls, state + 8);
            end = rd32(ls, state + 4);
        }
        wr32(ls, cur, 0x40001824u | (batches << 18));
        cur += 4;
        wr32(ls, state + 8, cur);
        if (rem > 256) {
            do {
                wr32(ls, cur, start | 0xff000000u);
                rem -= 256;
                start += 256;
                cur += 4;
            } while (rem > 256);
            wr32(ls, state + 8, cur);
        }
        if (rem != 0) {
            wr32(ls, cur, start | ((rem - 1) << 24));
            cur += 4;
            wr32(ls, state + 8, cur);
        }
        if (cur + 8 > end) {
            if (!hole_grow(j, state, 2)) return;
            cur = rd32(ls, state + 8);
        }
        wr32(ls, cur, 0x00041808u);
        wr32(ls, cur + 4, 0);
        cur += 8;
        wr32(ls, state + 8, cur);
    }
    edge::glue::end_command_buffer_hole(ls, j.m, ctx, state, hole_ea, alloc, 1);
}

void entry(Job& j) {
    std::memset(j.ls + 0x10b20, 0, 0x10c80 - 0x10b20);
    for (int k = 0; k < 4; k++) wr32(j.ls, kJobPtr + 4 * k, kApiLs);
    for (int k = 0; k < 4; k++) wr32(j.ls, 0x10b10 + 4 * k, 0);
    for (int k = 0; k < 4; k++) wr32(j.ls, 0x10b90 + 4 * k, 0);
    main_body(j);
}

}

bool enabled() { return g_on; }

void counters(uint64_t fn_fallbacks[3], uint64_t& restarts, uint64_t& late_restarts) {
    for (int k = 0; k < 3; k++) fn_fallbacks[k] = g_fn_fallbacks[k].load(std::memory_order_relaxed);
    restarts = g_restarts.load(std::memory_order_relaxed);
    late_restarts = g_late_restarts.load(std::memory_order_relaxed);
}

bool run(Executor& x, spu::Host& h, const JobRef&, const Cmd&) {
    Job j(x, h);
    uint64_t t0 = prof_on() ? __rdtsc() : 0;
    entry(j);
    if (prof_on()) prof_add("hle:job", __rdtsc() - t0);
    if (j.failed) {
        static std::atomic<int> logged{0};
        g_restarts.fetch_add(1, std::memory_order_relaxed);
        if (j.m.committed) {
            g_late_restarts.fetch_add(1, std::memory_order_relaxed);
            if (logged.fetch_add(1) < 20) LOG_ERROR("jsedge", "hle job failed after issuing DMA (%s), restarting it on the SPU glue", j.fail_what ? j.fail_what : "?");
        } else if (logged.fetch_add(1) < 20) {
            LOG_WARN("jsedge", "hle job declined (%s), running the SPU glue", j.fail_what ? j.fail_what : "?");
        }
        return false;
    }
    x.last_nv = j.nv;
    j.c.pc = kJobRet;
    return true;
}

}
