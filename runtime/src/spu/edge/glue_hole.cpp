#include "edge_glue.h"
#include "spu_float.h"

#include <cstring>

namespace edge::glue {

namespace {

constexpr uint32_t kHdrSemaphoreOffset = 0x00041d6c;
constexpr uint32_t kHdrBackEndRelease = 0x00041d74;
constexpr uint32_t kHdrFifoSemaphoreOffset = 0x00040064;
constexpr uint32_t kHdrFifoSemaphoreAcquire = 0x00040068;
constexpr uint32_t kHdrInvalidateL2 = 0x00041fd8;
constexpr uint32_t kHdrShaderProgram = 0x000408e4;
constexpr uint32_t kHdrShaderControl = 0x00041d60;
constexpr uint32_t kHdrTexCoordControl = 0x00280b40;
constexpr uint32_t kHdrTransformConstantLoad = 0x00141efc;
constexpr uint32_t kHdrScissor = 0x000808c0;
constexpr uint32_t kHdrViewportHorizontal = 0x00080a00;
constexpr uint32_t kHdrClipMin = 0x00080394;
constexpr uint32_t kHdrViewportOffset = 0x00200a20;
constexpr uint32_t kHdrInvalidateVertexCache = 0x00041710;
constexpr uint32_t kHdrInvalidateVertexFile = 0x00041714;
constexpr uint32_t kNopNonIncrement = 0x40000100;
constexpr uint32_t kHeaderKeepMask = 0xe003ffff;
constexpr uint32_t kCountMask = 0x1ffc0000;

bool hole_room(const uint8_t* ls, uint32_t state, uint32_t& cur, uint32_t& end, uint32_t bytes) {
    cur = rd32(ls, state + kHoleCur);
    end = rd32(ls, state + kHoleEnd);
    return !(cur + bytes > end);
}

void shift_block(uint8_t* ls, uint32_t b) {
    uint8_t tmp[132];
    for (uint32_t i = 0; i < 132; i++) tmp[i] = ls[(b - 4 + i) & kLsMask];
    for (uint32_t i = 0; i < 128; i++) ls[(b + i) & kLsMask] = tmp[i];
}

uint32_t split_at_lines(uint8_t* ls, uint32_t begin, uint32_t len) {
    uint32_t r9 = begin, r5 = 0, r20 = begin + (len & ~3u), r22 = len;
    uint32_t r3 = 0, r18 = 0, r25 = 0, r26 = 0;
    for (;;) {
        bool more = r20 > r5;
        bool prev_ok = r18 == r3;
        r18 = r9 & ~127u;
        if (!more) break;
        uint32_t r19 = r25, r10 = r5;
        r25 = rd32(ls, r9);
        uint32_t r16 = (r10 + 128) & ~127u;
        uint32_t r8 = r16 - (r10 + 4);
        uint32_t r39 = r8 >> 2;
        uint32_t r38 = ((r19 >> 28) & 4) ^ 4;
        uint32_t r27 = r26 - r8;
        r26 = (r25 >> 16) & 0x1ffc;
        uint32_t r28 = (r38 & 0xffff) * (r39 & 0xffff);
        uint32_t r37 = r9 + r26;
        r5 = r9;
        r3 = r37 & ~127u;
        r9 = r37 + 4;
        if (prev_ok) continue;
        uint32_t hdr1 = r8 == 0 ? 0 : (r19 & kHeaderKeepMask) | ((r8 << 16) & kCountMask);
        wr32(ls, r10, hdr1);
        uint32_t top = r16 + ((r20 - r16) & ~127u);
        for (uint32_t b = top;; b -= 128) {
            shift_block(ls, b);
            if (b == r16) break;
        }
        uint32_t hdr2 = ((r19 & kHeaderKeepMask) | ((r27 << 16) & kCountMask)) + r28;
        wr32(ls, r16, hdr2);
        r9 = r16;
        r5 = r16;
        r3 = r18;
        r20 += 4;
        r22 += 4;
    }
    return r22;
}

}

uint32_t hole_reserve(const uint8_t* ls, uint32_t state, uint32_t words) {
    (void)words;
    return rd32(ls, state + kHoleCallback) == 0 ? 1 : 0;
}

bool set_fragment_program(uint8_t* ls, uint32_t state, uint32_t prog, uint32_t control, uint32_t tex2d, uint32_t centroid) {
    uint32_t cur, end;
    if (!hole_room(ls, state, cur, end, 60)) return false;
    wr32(ls, cur, kHdrShaderProgram);
    wr32(ls, cur + 4, prog);
    wr32(ls, cur + 8, kHdrShaderControl);
    wr32(ls, cur + 12, control);
    wr32(ls, cur + 16, kHdrTexCoordControl);
    uint32_t a = tex2d, b = centroid << 4;
    for (uint32_t i = 0; i < 10; i++) {
        wr32(ls, cur + 20 + 4 * i, (a & 1) | (b & 16));
        a >>= 1;
        b >>= 1;
    }
    wr32(ls, state + kHoleCur, cur + 60);
    return true;
}

bool set_transform_constant(uint8_t* ls, uint32_t state, uint32_t slot, uint32_t src) {
    uint32_t cur, end;
    if (!hole_room(ls, state, cur, end, 24)) return false;
    wr32(ls, cur, kHdrTransformConstantLoad);
    wr32(ls, cur + 4, slot);
    for (uint32_t i = 0; i < 4; i++) wr32(ls, cur + 8 + 4 * i, rd32(ls, src + 4 * i));
    wr32(ls, state + kHoleCur, cur + 24);
    return true;
}

bool set_scissor(uint8_t* ls, uint32_t state, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t cur, end;
    if (!hole_room(ls, state, cur, end, 12)) return false;
    wr32(ls, cur, kHdrScissor);
    wr32(ls, cur + 4, (w << 16) | x);
    wr32(ls, cur + 8, (h << 16) | y);
    wr32(ls, state + kHoleCur, cur + 12);
    return true;
}

bool set_viewport(uint8_t* ls, uint32_t state, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t zmin, uint32_t zmax) {
    uint32_t cur, end;
    if (!hole_room(ls, state, cur, end, 60)) return false;
    spuf::CsrGuard guard;
    float half_w = spuf::fl(spuf::cuflt(w, 154));
    float half_h = spuf::fl(spuf::cuflt(h, 154));
    float fx = spuf::fl(spuf::cuflt(x, 155));
    float fy = spuf::fl(spuf::cuflt(y, 155));
    float fmin = spuf::fl(zmin), fmax = spuf::fl(zmax);
    float off_x = spuf::fa(half_w, fx);
    float off_y = spuf::fa(half_h, fy);
    float off_z = spuf::fm(spuf::fa(fmin, fmax), 0.5f);
    float scale_z = spuf::fm(spuf::fs(fmax, fmin), 0.5f);
    wr32(ls, cur, kHdrViewportHorizontal);
    wr32(ls, cur + 4, (w << 16) | x);
    wr32(ls, cur + 8, (h << 16) | y);
    wr32(ls, cur + 12, kHdrClipMin);
    wr32(ls, cur + 16, zmin);
    wr32(ls, cur + 20, zmax);
    wr32(ls, cur + 24, kHdrViewportOffset);
    wr32(ls, cur + 28, spuf::bits(off_x));
    wr32(ls, cur + 32, spuf::bits(off_y));
    wr32(ls, cur + 36, spuf::bits(off_z));
    wr32(ls, cur + 40, 0);
    wr32(ls, cur + 44, spuf::bits(half_w));
    wr32(ls, cur + 48, spuf::bits(half_h) ^ 0x80000000u);
    wr32(ls, cur + 52, spuf::bits(scale_z));
    wr32(ls, cur + 56, 0);
    wr32(ls, state + kHoleCur, cur + 60);
    return true;
}

bool set_instance_state(uint8_t* ls, uint32_t inst, uint32_t state) {
    uint32_t prog = rd32(ls, inst + 24);
    if (prog == 0) return true;
    uint32_t label = rd32(ls, inst + 20), value = rd32(ls, inst + 28);
    uint32_t cur, end;
    if (!hole_room(ls, state, cur, end, 16)) return false;
    wr32(ls, cur, kHdrSemaphoreOffset);
    wr32(ls, cur + 4, label & 0xff0);
    wr32(ls, cur + 8, kHdrBackEndRelease);
    wr32(ls, cur + 12, value);
    wr32(ls, state + kHoleCur, cur + 16);
    if (ls[(inst + 16) & kLsMask] != 0 && ls[(inst + 17) & kLsMask] != 0) {
        if (!hole_room(ls, state, cur, end, 16)) return false;
        wr32(ls, cur, kHdrFifoSemaphoreOffset);
        wr32(ls, cur + 4, label & 0xff0);
        wr32(ls, cur + 8, kHdrFifoSemaphoreAcquire);
        wr32(ls, cur + 12, value);
        wr32(ls, state + kHoleCur, cur + 16);
        cur += 16;
        if (cur + 8 > end) return false;
        wr32(ls, cur, kHdrInvalidateL2);
        wr32(ls, cur + 4, 1);
        wr32(ls, state + kHoleCur, cur + 8);
    }
    return set_fragment_program(ls, state, rd32(ls, inst + 24), rd32(ls, inst + 4), rd16(ls, inst + 2), rd16(ls, inst));
}

bool set_vertex_data_arrays(uint8_t* ls, uint32_t ctx, uint32_t state, uint32_t location, uint32_t io_offset, uint32_t entry, const OverlayHook& hook) {
    if (!hook.call) return false;
    const uint32_t regs[8] = {ctx, 0, state, location, io_offset, 0, kSetArraysBss, entry};
    return hook.call(hook.user, ls, entry, regs);
}

void begin_command_buffer_hole(uint8_t* ls, Mfc& m, uint32_t ctx, uint32_t state, uint32_t hole_ea, uint32_t allocs, uint32_t num_allocs) {
    uint32_t slot = ctx_scratch_ptr(ls, ctx);
    set_ctx_scratch_ptr(ls, ctx, slot + 16);
    uint32_t p = ctx_scratch_ptr(ls, ctx);
    uint32_t begin = (p & ~127u) | (hole_ea & 127);
    if (p > begin) begin += 128;
    wr32(ls, state + kHoleBegin, begin);
    wr32(ls, state + kHoleCur, begin);
    uint32_t hole_size = ls[(ctx + 1) & kLsMask];
    uint32_t end = begin + hole_size * 16 + (hole_size == 0 ? 4 : 0);
    wr32(ls, state + kHoleEnd, end);
    wr32(ls, state + kHoleCallback, 0);
    set_ctx_scratch_ptr(ls, ctx, (end + 127) & ~127u);
    for (uint32_t i = 0; i < num_allocs; i++) {
        uint32_t e = allocs + 32 * i;
        if (rd32(ls, e + 8) != 0) continue;
        m.getf(slot, rd32(ls, e), 4, rd32(ls, ctx + 336));
    }
    uint8_t flag = 0;
    for (uint32_t i = 0; i < num_allocs; i++) {
        uint32_t e = allocs + 32 * i;
        if (flag == 0 && rd32(ls, e + 16) != 0 && ls[(e + 28) & kLsMask] != 0) flag = ls[(e + 29) & kLsMask];
    }
    if (flag == 0) return;
    uint32_t cur = rd32(ls, state + kHoleCur);
    wr32(ls, state + kHoleCur, cur + 32);
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t w = i == 0 ? kHdrInvalidateVertexCache : (i & 1) ? 0 : kHdrInvalidateVertexFile;
        wr32(ls, cur + 4 * i, w);
    }
}

uint32_t end_command_buffer_hole(uint8_t* ls, Mfc& m, uint32_t ctx, uint32_t state, uint32_t hole_ea, uint32_t allocs, uint32_t num_allocs) {
    if (hole_ea == 0) return ctx;
    uint32_t cur = rd32(ls, state + kHoleCur);
    for (uint32_t i = 0; i < num_allocs; i++) {
        uint32_t e = allocs + 32 * i;
        uint32_t label = rd32(ls, e + 16);
        if (label == 0) continue;
        wr32(ls, cur, kHdrSemaphoreOffset);
        wr32(ls, cur + 4, label & 0xff0);
        wr32(ls, cur + 8, kHdrBackEndRelease);
        wr32(ls, cur + 12, rd32(ls, e + 12));
        cur += 16;
    }
    wr32(ls, state + kHoleCur, cur);
    uint32_t begin = rd32(ls, state + kHoleBegin);
    uint32_t len = cur - begin;
    uint32_t new_len = len ? split_at_lines(ls, begin, len) : 0;
    cur = rd32(ls, state + kHoleCur) + ((new_len - len) & ~3u);
    wr32(ls, state + kHoleCur, cur);
    uint32_t end = rd32(ls, state + kHoleEnd);
    uint32_t hole_size = ls[(ctx + 1) & kLsMask];
    uint32_t bytes = hole_size ? hole_size * 16 : 4;
    int32_t n = int32_t(end - cur) >> 2;
    if (n != 0) {
        wr32(ls, cur, kNopNonIncrement | (uint32_t(n - 1) << 18));
        cur = cur + (uint32_t(n - 1) << 2) + 4;
        wr32(ls, state + kHoleCur, cur);
    }
    begin = rd32(ls, state + kHoleBegin);
    m.put(begin, hole_ea, bytes, rd32(ls, ctx + 336), true);
    return bytes;
}

}
