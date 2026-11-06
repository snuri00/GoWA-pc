#include "edge_adapters.h"

#include "edge_cull.h"
#include "edge_cull_gen.h"
#include "edge_gpu.h"
#include "edge_skin.h"
#include "edge_softbody.h"
#include "edge_vcomp.h"
#include "edge_vdecomp.h"
#include "edge_xform.h"
#include "spu_float.h"

#include <cstdlib>
#include <cstring>

namespace edge {

namespace ad_common {

const uint32_t kEdgeHeader[] = {0x43642a02u, 0x42370282u, 0x420c0702u, 0x43499982u, 0x00006d98u, 0x0000cb20u, 0x00000160u, 0x00000010u, 0xc0dec0deu, 0x00000008u, 0x00000000u, 0x00000005u};

uint32_t rbe32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

void store_quad(uint8_t* ls, uint32_t a, const spu::v128& v) {
    a &= 0x3fff0;
    for (int i = 0; i < 4; i++) {
        uint32_t w = v.u32[3 - i];
        ls[a + 4 * i] = uint8_t(w >> 24);
        ls[a + 4 * i + 1] = uint8_t(w >> 16);
        ls[a + 4 * i + 2] = uint8_t(w >> 8);
        ls[a + 4 * i + 3] = uint8_t(w);
    }
}

void store_words(uint8_t* ls, uint32_t a, const uint32_t w[4], unsigned mask) {
    a &= 0x3fff0;
    for (int i = 0; i < 4; i++) {
        if (!(mask & (1u << i))) continue;
        ls[a + 4 * i] = uint8_t(w[i] >> 24);
        ls[a + 4 * i + 1] = uint8_t(w[i] >> 16);
        ls[a + 4 * i + 2] = uint8_t(w[i] >> 8);
        ls[a + 4 * i + 3] = uint8_t(w[i]);
    }
}

void save_lr(uint8_t* ls, const spu::Context& c) { store_quad(ls, c.gpr[1].u32[3] + 16, c.gpr[0]); }

}

namespace {

struct HeaderBytes {
    uint8_t b[48];
    HeaderBytes() {
        for (int i = 0; i < 12; i++)
            for (int k = 0; k < 4; k++) b[4 * i + k] = uint8_t(ad_common::kEdgeHeader[i] >> (24 - 8 * k));
    }
};

const HeaderBytes kHeaderBytes;

}

bool image_loaded(const uint8_t* ls) { return std::memcmp(ls + 0x4000, kHeaderBytes.b, sizeof kHeaderBytes.b) == 0; }

namespace ad_index {

bool dcb0(uint8_t* ls, spu::Context& c, Outputs& outs) {
    uint32_t src = c.gpr[4].u32[3], sp = c.gpr[1].u32[3] & 0x3fff0;
    if (sp < 0x2000 || !decompress_indexes_safe(ls, c.gpr[3].u32[3] & 0x3fff0, src, sp - 0x1000)) return false;
    if (gpu::g_mix) gpu::mix_note("I", -1, 0);
    decompress_indexes(ls, c.gpr[3].u32[3] & 0x3fff0, c.gpr[4].u32[3], &outs);
    return true;
}

}

namespace ad_cull {

template <void (*F)(xo::V*, uint8_t*, xo::Ctl&)>
bool run_gen(uint8_t* ls, spu::Context& c, Outputs&) {
    spuf::CsrGuard guard;
    alignas(16) xo::V R[128];
    std::memcpy(R, c.gpr, sizeof R);
    xo::Ctl ctl;
    F(R, ls, ctl);
    if (ctl.fail) return false;
    std::memcpy(c.gpr, R, sizeof R);
    return true;
}

void be_quad(uint8_t* out, const spu::v128& v) { _mm_storeu_si128(reinterpret_cast<__m128i*>(out), xo::bswap(v.vi)); }

bool d618(uint8_t* ls, spu::Context& c, Outputs& outs) {
    static const bool force_gen = std::getenv("EDGE_CULL_GEN") != nullptr;
    CullIn in;
    in.ctx = c.gpr[3].u32[3];
    be_quad(in.r0, c.gpr[0]);
    be_quad(in.r1, c.gpr[1]);
    be_quad(in.r4, c.gpr[4]);
    be_quad(in.r5, c.gpr[5]);
    be_quad(in.r80, c.gpr[80]);
    be_quad(in.r81, c.gpr[81]);
    be_quad(in.r82, c.gpr[82]);
    be_quad(in.r126, c.gpr[126]);
    if (gpu::mode()) gpu::touch_all(ls, 5);
    if (gpu::g_mix) gpu::mix_note("C", -1, 0);
    int32_t n = force_gen ? -1 : cull_triangles(ls, in);
    if (n < 0) return run_gen<xo::f_0d618>(ls, c, outs);
    c.gpr[3].vi = _mm_set1_epi32(n);
    return true;
}

bool x73e0(uint8_t* ls, spu::Context& c, Outputs& outs) {
    static const bool use_gen = std::getenv("EDGE_SB_GEN") != nullptr;
    if (gpu::mode()) gpu::touch_all(ls, 6);
    if (gpu::g_mix) gpu::mix_note("Z", -1, 0);
    if (use_gen) return run_gen<xo::f_073e0>(ls, c, outs);
    uint32_t sp = c.gpr[1].u32[3];
    if ((sp & 15) || sp < 0x2000 || sp > 0x3ffc0 || c.gpr[11].u32[3] > 0x2000) return false;
    if ((c.gpr[3].u32[3] & 0x3fff0) < 0x10000 || (c.gpr[3].u32[3] & 0x3fff0) + 0x200 > 0x40000) return false;
    const int rs[] = {4, 5, 6, 7, 9, 10, 12};
    for (int r : rs) {
        uint32_t a = c.gpr[r].u32[3];
        if (a == 0 && (r == 5 || r == 6 || r == 7)) continue;
        if ((a & 15) || a < 0x10c80 || a >= sp - 0x1000) return false;
    }
    return soft_body(ls, c.gpr);
}

}

namespace ad_skin {

bool e550(uint8_t* ls, spu::Context& c, Outputs&) {
    using namespace ad_common;
    uint32_t ctx = c.gpr[3].u32[3] & 0x3fff0;
    uint32_t sp = c.gpr[1].u32[3];
    if ((sp & 15) || sp < 0x1000 || sp > 0x3ffc0 || ctx < 0x10000 || ctx + 0x200 > 0x40000) return false;
    if (gpu::mode()) gpu::touch_all(ls, 4);
    if (gpu::g_mix) gpu::mix_note("S", -1, 0);
    uint32_t q96[4];
    for (int i = 0; i < 4; i++) q96[i] = rbe32(ls + ctx + 96 + 4 * i);
    if (!skin_vertexes(ls, ctx, c.gpr[4].u32[3], c.gpr[5].u32[3], c.gpr[6].u32[3], sp - 0x1000)) return false;
    save_lr(ls, c);
    for (int r = 80; r <= 102; r++) store_quad(ls, sp - 16 * uint32_t(r - 79), c.gpr[r]);
    store_quad(ls, sp - 384, c.gpr[126]);
    store_quad(ls, sp - 656, c.gpr[1]);
    uint32_t fr = sp - 656;
    uint32_t q100[4] = {q96[1], q96[2], q96[3], q96[0]};
    uint32_t q104[4] = {q96[2], q96[3], q96[0], q96[1]};
    store_words(ls, fr + 32, q100, 15);
    store_words(ls, fr + 32 + (q96[1] ? 16 : 0), q104, 15);
    return true;
}

}

namespace ad_vcomp {

struct Sig {
    int id;
    uint32_t size;
    uint64_t head;
    uint64_t hash;
};

const Sig kSigs[] = {
    {0, 0x150, 0x9e8105341242001cull, 0x7a978ce86a4ae537ull},
    {4, 0x940, 0xd0c0ff24ab01041cull, 0x5984bcb4ced99e01ull},
    {7, 0x450, 0xc38105349f01041cull, 0x0a95e932a57168f5ull},
    {3, 0x7c0, 0xd0c0ff2439001c41ull, 0x28d85cde44accd9bull},
    {6, 0x350, 0x828105340442001cull, 0x2141150a87839f69ull},
    {21, 0x100, 0xc8810534bb01041cull, 0x55759048cd2f18c3ull},
    {22, 0x100, 0xb78105342c42001cull, 0xf6b5d50af651d52aull},
    {23, 0x100, 0xb5810534a901041cull, 0x14de2d771e913763ull},
    {24, 0x100, 0x9f810534a301041cull, 0x4c68222f0f78f3beull},
    {26, 0x100, 0xbb8105349401041cull, 0x50fe487ceb49936cull},
    {28, 0x100, 0x8a810534b47f2341ull, 0x4776727a88c141d3ull},
};

int identify(const uint8_t* ls, uint32_t entry) {
    uint64_t head;
    std::memcpy(&head, ls + entry, 8);
    for (const Sig& s : kSigs) {
        if (s.head != head || entry + s.size > 0x40000) continue;
        if (overlay_matches(ls + entry, s.size, s.hash)) return s.id;
    }
    return -1;
}

bool d158(uint8_t* ls, spu::Context& c, Outputs& outs) {
    uint32_t ctx = c.gpr[3].u32[3] & 0x3fff0;
    uint32_t cb = ad_common::rbe32(ls + ctx + 140) & 0x3fffc;
    int id = -1;
    if (cb) {
        id = identify(ls, cb);
        if (id < 0) return false;
    }
    uint32_t r3[4];
    bool called = false;
    uint32_t sp = c.gpr[1].u32[3] & 0x3fff0;
    if (sp < 0x2000) return false;
    if (gpu::mode()) gpu::before_compress(ls, ctx, id);
    if (gpu::g_mix) gpu::mix_note("K", id, 0);
    if (!compress_vertexes(ls, ctx, id, r3, &outs, &called, sp - 0x1000)) return false;
    if (gpu::mode()) gpu::after_compress(ls, ctx);
    uint32_t regs[128][4];
    for (int r = 0; r < 128; r++)
        for (int i = 0; i < 4; i++) regs[r][i] = c.gpr[r].u32[3 - i];
    compress_vertexes_stack(ls, regs, id, called, r3);
    for (int i = 0; i < 4; i++) c.gpr[3].u32[3 - i] = r3[i];
    return true;
}

}

namespace ad_vdecomp {

bool dbc8(uint8_t* ls, spu::Context& c, Outputs& outs) {
    if ((c.gpr[3].u32[3] & 0x3fff0) + 0x200 > 0x40000) return false;
    uint32_t ctx = c.gpr[3].u32[3] & 0x3fff0;
    uint32_t r8 = c.gpr[8].u32[3] & 0x3fffc;
    uint32_t nv = (ls[ctx + 8] << 8) | ls[ctx + 9];
    if (!nv || !r8) return true;
    int ov = vdecomp_identify(ls, r8);
    if (ov < 0) return false;
    if (gpu::g_mix) gpu::mix_note("D", ov, nv);
    VdRegs regs;
    auto put = [&](uint8_t* d, int r) {
        for (int j = 0; j < 4; j++) {
            uint32_t w = c.gpr[r].u32[3 - j];
            d[4 * j + 0] = uint8_t(w >> 24);
            d[4 * j + 1] = uint8_t(w >> 16);
            d[4 * j + 2] = uint8_t(w >> 8);
            d[4 * j + 3] = uint8_t(w);
        }
    };
    put(regs.r1, 1);
    for (int i = 0; i < 9; i++) put(regs.saved[i], 80 + i);
    put(regs.saved[9], 126);
    if (gpu::mode()) {
        gpu::job_start();
        if (ov == 10 && gpu::defer_decompress(ls, ctx, c.gpr[4].u32[3] & 0x3ffff, nv, c.gpr[5].u32[3] & 0x3ffff)) {
            c.gpr[3] = spu::v128{};
            return true;
        }
    }
    if (!vdecomp_run(ls, ov, ctx, c.gpr[4].u32[3], nv, c.gpr[5].u32[3], c.gpr[6].u32[3] & 255, regs, &outs)) {
        if (gpu::mode()) gpu::job_start();
        return false;
    }
    if (gpu::mode()) gpu::poison_table(ls);
    c.gpr[3] = spu::v128{};
    return true;
}

}

namespace ad_xform {

using namespace ad_common;

void set_r3(spu::Context& c, const uint32_t q[4]) {
    for (int i = 0; i < 4; i++) c.gpr[3].u32[3 - i] = q[i];
}

bool f538(uint8_t* ls, spu::Context& c, Outputs&) {
    uint32_t q[4];
    uint32_t n = c.gpr[3].u32[3];
    if (!n) return true;
    if (gpu::g_mix) gpu::mix_note("T", -1, 0);
    if (gpu::mode() && gpu::defer_transform(ls, n, c.gpr[4].u32[3] & 0x3ffff, c.gpr[5].u32[3] & 0x3ffff, c.gpr[6].u32[3] & 0x3fff0, q)) {
        set_r3(c, q);
        return true;
    }
    transform_vertexes(ls, n, c.gpr[4].u32[3], c.gpr[5].u32[3], c.gpr[6].u32[3], q);
    set_r3(c, q);
    return true;
}

bool a880(uint8_t* ls, spu::Context& c, Outputs&) {
    if ((c.gpr[3].u32[3] & 0x3fff0) + 0x200 > 0x40000 || c.gpr[1].u32[3] < 0x1000 || c.gpr[1].u32[3] > 0x3ffc0) return false;
    uint32_t q[4];
    uint32_t sp = c.gpr[1].u32[3];
    store_quad(ls, sp - 16, c.gpr[80]);
    store_quad(ls, sp - 32, c.gpr[81]);
    store_quad(ls, sp - 48, c.gpr[82]);
    store_quad(ls, sp - 64, c.gpr[83]);
    store_quad(ls, sp - 80, c.gpr[126]);
    save_lr(ls, c);
    store_quad(ls, sp - 112, c.gpr[1]);
    uint32_t ctx = c.gpr[3].u32[3] & 0x3fff0;
    if (gpu::g_mix) gpu::mix_note("B", -1, 0);
    if (rbe32(ls + ctx + 100) && rbe32(ls + ctx + 104) && !rbe32(ls + ctx + 96)) {
        spu::v128 lr{}, r1 = c.gpr[1];
        lr.u32[3] = 0xad88;
        for (int i = 0; i < 4; i++) r1.u32[i] -= 112;
        store_quad(ls, sp - 96, lr);
        store_quad(ls, sp - 128, c.gpr[3]);
        store_quad(ls, sp - 160, r1);
    }
    compute_binormals(ls, ctx, int32_t(c.gpr[4].u32[3]), q);
    set_r3(c, q);
    return true;
}

bool bbc8(uint8_t* ls, spu::Context& c, Outputs&) {
    if (c.gpr[4].u32[3] > 0x10000) return false;
    if (gpu::mode()) gpu::touch(ls, c.gpr[3].u32[3] & 0x3fff0, (c.gpr[3].u32[3] & 0x3fff0) + 16 * ((c.gpr[4].u32[3] + 7) & ~7u), 7);
    if (gpu::g_mix) gpu::mix_note("N", -1, 0);
    uint32_t q[4], n[4];
    for (int i = 0; i < 4; i++) n[i] = c.gpr[4].u32[3 - i];
    if (!normalize_vertexes(ls, c.gpr[3].u32[3], n, q)) return false;
    set_r3(c, q);
    return true;
}

bool x88b0(uint8_t* ls, spu::Context& c, Outputs&) {
    if ((c.gpr[3].u32[3] & 0x3fff0) + 0x200 > 0x40000 || c.gpr[1].u32[3] < 0x1000 || c.gpr[1].u32[3] > 0x3ffc0) return false;
    if (gpu::g_mix) gpu::mix_job();
    {
        uint32_t spv = c.gpr[1].u32[3];
        const int rs[] = {3, 4, 5, 6, 7, 15, 16, 17};
        for (int r : rs) {
            uint32_t a = c.gpr[r].u32[3] & 0x3ffff;
            if (a + 0x200 > spv - 0x1000 && a < spv + 0x40) return false;
        }
    }
    if ((c.gpr[1].u32[3] & 15) || (c.gpr[4].u32[3] & 15) || (c.gpr[3].u32[3] & 15)) return false;
    SegmentInitArgs a{};
    for (int r = 3; r < 20; r++) a.r[r] = c.gpr[r].u32[3];
    uint32_t sp = c.gpr[1].u32[3];
    for (int r = 81; r <= 93; r++) store_quad(ls, sp - 16 * (r - 79), c.gpr[r]);
    store_quad(ls, sp - 240, c.gpr[126]);
    save_lr(ls, c);
    store_quad(ls, sp - 16, c.gpr[80]);
    store_quad(ls, sp - 320, c.gpr[1]);
    uint32_t f = sp - 320;
    uint32_t w0[4] = {0x10a30, 0, 0x109a0, 0}, w1[4] = {a.r[18], 0, 0, 0}, w2[4] = {a.r[19], 0, 0, 0};
    store_words(ls, f + 32, w0, 15);
    store_words(ls, f + 48, w1, 15);
    store_words(ls, f + 64, w2, 3);
    init_segment(ls, a);
    return true;
}

}

namespace {

const Adapter kAdapters[] = {
    {0x88b0, 16, 0x5d0a11d40010e23aull, ad_xform::x88b0, "Edge.init_segment"},
    {0xdbc8, 16, 0x8740271656fd3972ull, ad_vdecomp::dbc8, "Edge.decompress_vertexes"},
    {0xe550, 16, 0x1d8f43fcb2904dc3ull, ad_skin::e550, "Edge.skin_vertexes"},
    {0xf538, 16, 0x6310cae3fbca748aull, ad_xform::f538, "Edge.transform_vertexes"},
    {0xbbc8, 16, 0x281641360126d6a3ull, ad_xform::bbc8, "Edge.normalize_vertexes"},
    {0xdcb0, 16, 0x3724604973ce4941ull, ad_index::dcb0, "Edge.decompress_indexes"},
    {0xd618, 16, 0xb25bcc18f19d2b48ull, ad_cull::d618, "Edge.cull_triangles"},
    {0xa880, 16, 0x18013234e75d75bfull, ad_xform::a880, "Edge.compute_binormals"},
    {0x73e0, 16, 0x70a6fb27bb7257a2ull, ad_cull::x73e0, "Edge.soft_body"},
    {0xd158, 16, 0x93732be09d752be1ull, ad_vcomp::d158, "Edge.compress_vertexes"},
};

}

const Adapter* adapters(size_t& count) {
    count = sizeof(kAdapters) / sizeof(kAdapters[0]);
    return kAdapters;
}

}
