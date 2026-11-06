#include "glue_reg.h"
#include "natreg.h"
#include "edge_cull_gen.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace edge::sb {
void f_07040(xo::V* R, uint8_t* ls, xo::Ctl& ctl);
}

namespace {

using edge::glue::Mfc;

uint32_t arg(const spu::Context& c, int r) { return c.gpr[r].u32[3]; }

void ret(spu::Context& c, uint32_t v) { c.gpr[3].u32[3] = v; }

void save_link(uint8_t* ls, const spu::Context& c) {
    uint32_t a = (arg(c, 1) + 16) & 0x3fff0;
    for (int i = 0; i < 4; i++) edge::glue::wr32(ls, a + 4 * i, c.gpr[0].u32[3 - i]);
}

bool p_5d90(uint8_t* ls, spu::Context& c, Mfc&) {
    ret(c, edge::glue::clear_quads(ls, arg(c, 3), arg(c, 4)));
    return true;
}

bool p_5de8(uint8_t* ls, spu::Context& c, Mfc&) {
    save_link(ls, c);
    ret(c, edge::glue::next_uniform_table(ls, arg(c, 3)));
    return true;
}

bool p_5e30(uint8_t*, spu::Context& c, Mfc& m) {
    edge::glue::tag_wait(m, arg(c, 3));
    ret(c, 2);
    return true;
}

bool p_5e80(uint8_t* ls, spu::Context& c, Mfc& m) {
    save_link(ls, c);
    ret(c, edge::glue::wait_label_outside(ls, m, arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 6), arg(c, 7), arg(c, 8), arg(c, 1)));
    return true;
}

bool p_5f88(uint8_t* ls, spu::Context& c, Mfc& m) {
    save_link(ls, c);
    ret(c, edge::glue::ringb_alloc(ls, m, arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 6), arg(c, 7), arg(c, 1)));
    return true;
}

bool p_67c0(uint8_t* ls, spu::Context& c, Mfc& m) {
    save_link(ls, c);
    ret(c, edge::glue::ringb_reserve(ls, m, arg(c, 3), arg(c, 4), arg(c, 5)));
    return true;
}

bool p_61b0(uint8_t* ls, spu::Context& c, Mfc& m) {
    save_link(ls, c);
    edge::glue::ringb_commit(ls, m, arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 1));
    return true;
}

bool p_9a60(uint8_t* ls, spu::Context& c, Mfc& m) {
    uint32_t r3 = 0;
    if (!edge::glue::softprep_9a60(ls, m, arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 6), arg(c, 7), arg(c, 1), r3)) return false;
    ret(c, r3);
    return true;
}

bool p_7040(uint8_t* ls, spu::Context& c, Mfc&) {
    uint32_t scale[4] = {c.gpr[7].u32[3], c.gpr[7].u32[2], c.gpr[7].u32[1], c.gpr[7].u32[0]};
    edge::glue::asym_deform(ls, arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 6), scale);
    return true;
}

GluePortReg reg_5d90(0x5d90, "clear_quads", p_5d90, true);
GluePortReg reg_5de8(0x5de8, "next_uniform_table", p_5de8, true);
GluePortReg reg_5e30(0x5e30, "tag_wait", p_5e30, true);
GluePortReg reg_5e80(0x5e80, "wait_label_outside", p_5e80, true);
GluePortReg reg_5f88(0x5f88, "ringb_alloc", p_5f88, true);
GluePortReg reg_67c0(0x67c0, "ringb_reserve", p_67c0, true);
GluePortReg reg_61b0(0x61b0, "ringb_commit", p_61b0, false);
GluePortReg reg_9a60(0x9a60, "softbody_prepare", p_9a60, true);
GluePortReg reg_7040(0x7040, "asym_deform", p_7040, false);

uint32_t rand_float_bits(std::mt19937& rng) {
    uint32_t kind = rng() % 16;
    if (kind == 0) return rng();
    if (kind == 1) return (rng() & 0x807fffffu) | (uint32_t(rng() % 4 + 0x7c) << 23);
    if (kind == 2) return 0;
    if (kind == 3) return 0x80000000u;
    float f = (float(rng() % 200001) - 100000.0f) / 1000.0f;
    if (kind < 8) f *= 0.001f;
    return std::bit_cast<uint32_t>(f);
}

int cmd_x7040(const std::vector<std::string>& args) {
    int iters = args.size() > 1 ? std::stoi(args[1]) : 2000;
    std::mt19937 rng(12345);
    std::vector<uint8_t> a(spu::LS_SIZE + 16), b(spu::LS_SIZE + 16);
    int fail = 0;
    bool nice = args.size() > 2 && args[2] == "nice";
    std::vector<uint8_t> orig(spu::LS_SIZE + 16);
    for (int it = 0; it < iters && fail < 5; it++) {
        for (size_t i = 0; i < spu::LS_SIZE; i += 4) {
            uint32_t v = rand_float_bits(rng);
            while (nice && ((v & 0x7f800000u) == 0x7f800000u || (v & 0x7f800000u) == 0 || (v & 0x7f800000u) > 0x4f000000u)) v = rand_float_bits(rng);
            edge::glue::wr32(a.data(), uint32_t(i), v);
        }
        std::memcpy(a.data() + 0x4000, harness::elf().data() + 0x727580, 0xc430);
        uint32_t count = rng() % 40 + 1;
        uint32_t blk = (0x11000 + 16 * (rng() % 64)) & 0x3fff0;
        uint32_t in = 0x12000 + 16 * (rng() % 256);
        uint32_t out = (rng() % 4) ? 0x20000 + 16 * (rng() % 256) : in + 16 * (rng() % 8);
        if (rng() % 8 == 0) in += rng() % 16;
        if (rng() % 8 == 0) out += rng() % 16;
        if (rng() % 2) std::memset(a.data() + 0x10b90, 0, 16);
        b = a;
        orig = a;
        alignas(16) edge::xo::V R[128];
        std::memset(R, 0, sizeof R);
        R[1] = edge::xo::il(0x3f000);
        R[3] = edge::xo::il(int32_t(out));
        R[4] = edge::xo::il(int32_t(in));
        R[5] = edge::xo::il(int32_t(count));
        R[6] = edge::xo::il(int32_t(blk));
        R[7] = edge::xo::lq(a.data(), 0x30000 + 16 * (rng() % 64));
        alignas(16) uint32_t r7[4];
        _mm_store_si128(reinterpret_cast<__m128i*>(r7), R[7]);
        uint32_t scale[4] = {r7[3], r7[2], r7[1], r7[0]};
        {
            spuf::CsrGuard guard;
            edge::xo::Ctl ctl;
            edge::sb::f_07040(R, a.data(), ctl);
        }
        edge::glue::asym_deform(b.data(), out, in, count, blk, scale);
        size_t diff = 0;
        uint32_t first = 0;
        for (size_t i = 0; i < spu::LS_SIZE; i++)
            if (a[i] != b[i]) {
                if (!diff) first = uint32_t(i);
                diff++;
            }
        if (diff) {
            fail++;
            std::printf("  iter %d: diff %zu bytes first %05x (out %05x in %05x count %u blk %05x)\n", it, diff, first, out, in, count, blk);
            if (std::getenv("X7040_DEBUG")) {
                uint32_t q = first & 0x3fff0;
                uint32_t k = (q - (out & 0x3fff0)) / 16;
                std::printf("    gen  %08x %08x %08x %08x\n", edge::glue::rd32(a.data(), q), edge::glue::rd32(a.data(), q + 4), edge::glue::rd32(a.data(), q + 8), edge::glue::rd32(a.data(), q + 12));
                std::printf("    port %08x %08x %08x %08x\n", edge::glue::rd32(b.data(), q), edge::glue::rd32(b.data(), q + 4), edge::glue::rd32(b.data(), q + 8), edge::glue::rd32(b.data(), q + 12));
                std::printf("    old  %08x %08x %08x %08x\n", edge::glue::rd32(orig.data(), q), edge::glue::rd32(orig.data(), q + 4), edge::glue::rd32(orig.data(), q + 8), edge::glue::rd32(orig.data(), q + 12));
                uint32_t iq = (in & 0x3fff0) + 16 * k;
                std::printf("    in   %08x %08x %08x %08x thr %08x %08x %08x %08x scale %08x %08x %08x %08x\n", edge::glue::rd32(b.data(), iq), edge::glue::rd32(b.data(), iq + 4), edge::glue::rd32(b.data(), iq + 8), edge::glue::rd32(b.data(), iq + 12), edge::glue::rd32(b.data(), 0x10b90), edge::glue::rd32(b.data(), 0x10b94), edge::glue::rd32(b.data(), 0x10b98), edge::glue::rd32(b.data(), 0x10b9c), scale[0], scale[1], scale[2], scale[3]);
                for (uint32_t r = 0; r < 6; r++) std::printf("    m%u  %08x %08x %08x\n", r, edge::glue::rd32(b.data(), blk + 60 + 12 * r), edge::glue::rd32(b.data(), blk + 64 + 12 * r), edge::glue::rd32(b.data(), blk + 68 + 12 * r));
            }
        }
    }
    std::printf("x7040: %d iterations, %d failures\n", iters, fail);
    return fail ? 1 : 0;
}

CommandReg reg_x7040("x7040", cmd_x7040);

}
