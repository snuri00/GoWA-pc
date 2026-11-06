#include "edge_glue.h"

#include <cstring>

namespace edge::glue {

namespace {

constexpr uint32_t kQuadMask = 0x3fff0;
constexpr uint32_t kRingBChunk = 0x1400;
constexpr uint32_t kRingBAllocPad = 527;
constexpr int kStallDecReads = 1600;

inline uint32_t quad_rd32(const uint8_t* ls, uint32_t a) {
    uint32_t base = a & kQuadMask;
    uint32_t v = 0;
    for (uint32_t i = 0; i < 4; i++) v = (v << 8) | ls[base + ((a + i) & 15)];
    return v;
}

inline uint32_t quad_rd16(const uint8_t* ls, uint32_t a) {
    uint32_t base = a & kQuadMask;
    return (uint32_t(ls[base + (a & 15)]) << 8) | ls[base + ((a + 1) & 15)];
}

inline void wr8(uint8_t* ls, uint32_t a, uint8_t v) { ls[a & kLsMask] = v; }

inline void copy_quad(uint8_t* ls, uint32_t dst, uint32_t src) { std::memmove(ls + (dst & kQuadMask), ls + (src & kQuadMask), 16); }

inline uint32_t tag_mask(uint32_t tag) {
    tag &= 63;
    return tag < 32 ? 1u << tag : 0;
}

inline uint32_t align16(uint32_t v) { return (v + 15) & ~15u; }

inline uint32_t align128(uint32_t v) { return (v + 127) & ~127u; }

inline uint32_t ring_line_ea(const uint8_t* ls, uint32_t params) { return rd32(ls, params) + ((spu_ring_index(ls) + 1) << 7); }

}

uint32_t clear_quads(uint8_t* ls, uint32_t dst, uint32_t count) {
    if (int32_t(count) <= 0) return dst;
    uint32_t n = 0;
    do {
        n += 8;
        for (uint32_t i = 0; i < 8; i++) std::memset(ls + ((dst + 16 * i) & kQuadMask), 0, 16);
        dst += 128;
    } while (int32_t(count) > int32_t(n));
    return dst;
}

uint32_t next_uniform_table(const uint8_t* ls, uint32_t ctx) { return ctx_uniform_table(ls, ctx, ls[ctx & kQuadMask] & 15); }

void tag_wait(Mfc& m, uint32_t mask) { m.wait_tag(mask); }

uint32_t wait_label_outside(uint8_t* ls, Mfc& m, uint32_t tag, uint32_t label_ea, uint32_t lo1, uint32_t lo2, uint32_t hi1, uint32_t hi2, uint32_t sp) {
    for (;;) {
        uint32_t v = read_label(ls, m, tag, label_ea, sp - 144);
        if (lo1 <= v && v <= hi1) return v;
        if (lo2 <= v && v <= hi2) return v;
        for (int i = 0; i < kStallDecReads; i++) m.decrementer();
    }
}

uint32_t ringb_alloc(uint8_t* ls, Mfc& m, uint32_t tag, uint32_t label_ea, uint32_t out_ptr, uint32_t req, uint32_t line, uint32_t sp) {
    uint32_t size = (req + kRingBAllocPad) & ~127u;
    uint32_t start = rd32(ls, line + 32);
    uint32_t end = start + rd32(ls, line + 36);
    uint32_t fence = rd32(ls, line + 40);
    uint32_t cur = rd32(ls, line + 44);
    uint32_t pos;
    if (fence > cur) {
        pos = cur;
    } else {
        if (end - cur > size) {
            wr32(ls, out_ptr, cur);
            wr32(ls, line + 44, cur + size);
            return cur;
        }
        wr32(ls, line + 44, start);
        pos = start;
    }
    if (fence - pos > size) {
        wr32(ls, out_ptr, pos);
        uint32_t d = rd32(ls, line + 44);
        wr32(ls, line + 44, d + size);
        return d;
    }
    uint32_t result, lo1, lo2, hi1, hi2;
    if (end - pos > size) {
        result = pos;
        lo1 = pos + 1 + size;
        hi1 = end;
        lo2 = pos != start ? start : lo1;
        hi2 = pos != start ? pos : end;
        wr32(ls, out_ptr, pos);
        wr32(ls, line + 44, rd32(ls, line + 44) + size);
    } else {
        result = start;
        lo1 = start + 1 + size;
        lo2 = lo1;
        hi1 = pos;
        hi2 = pos;
        wr32(ls, out_ptr, start);
        wr32(ls, line + 44, rd32(ls, line + 32) + size);
    }
    uint32_t label = wait_label_outside(ls, m, tag, label_ea, lo1, lo2, hi1, hi2, sp - 80);
    wr32(ls, line + 40, label);
    return result;
}

uint32_t ringb_reserve(uint8_t* ls, Mfc& m, uint32_t ctx, uint32_t blk, uint32_t params) {
    wr32(ls, blk + 24, 0);
    uint32_t s16 = align16(ctx_scratch_ptr(ls, ctx));
    wr32(ls, blk + 44, s16 + 16);
    if (rd32(ls, params + 48) == 0) return 0;
    uint32_t line = (s16 + 143) & ~127u;
    wr32(ls, blk + 48, line);
    wr8(ls, blk + 17, 0);
    set_ctx_scratch_ptr(ls, ctx, line + 128);
    uint32_t line_ea = ring_line_ea(ls, params);
    uint32_t buf = ctx_scratch_ptr(ls, ctx);
    uint32_t sz56 = rd16(ls, params + 56);
    uint32_t sz60 = rd16(ls, params + 60);
    uint32_t a60 = align16(sz60);
    uint32_t c56 = sz56 <= kRingBChunk ? align16(sz56) : kRingBChunk;
    uint32_t nxt = align128(buf + a60);
    set_ctx_scratch_ptr(ls, ctx, nxt + c56);
    wr32(ls, blk + 36, nxt);
    wr32(ls, blk + 40, buf);
    m.getllar(line, line_ea);
    wr32(ls, blk + 20, rd32(ls, line + 28));
    m.get(buf, rd32(ls, params + 52), a60, rd32(ls, ctx + 340));
    wr8(ls, blk + 16, 1);
    return 1;
}

void ringb_commit(uint8_t* ls, Mfc& m, uint32_t ctx, uint32_t blk, uint32_t params, uint32_t sp) {
    uint32_t tag_get = rd32(ls, ctx + 340);
    uint32_t tag_put = rd32(ls, ctx + 336);
    uint32_t mask = tag_mask(tag_get);
    m.wait_tag(mask);
    uint32_t sz56 = rd16(ls, params + 56);
    uint32_t src = rd32(ls, params + 48);
    uint32_t nxt = rd32(ls, blk + 36);
    uint32_t fetched = sz56 <= kRingBChunk ? sz56 : kRingBChunk;
    get_chunked(m, nxt, src, fetched, tag_get);
    m.wait_tag(mask);
    wr16(ls, blk, rd16(ls, nxt + 4));
    uint32_t line = rd32(ls, blk + 48);
    wr16(ls, blk + 2, rd16(ls, nxt + 6));
    wr32(ls, blk + 4, rd32(ls, nxt + 8));
    wr32(ls, blk + 12, rd32(ls, nxt));
    uint32_t hdr = rd32(ls, nxt + 16);
    uint32_t data = nxt + hdr;
    uint32_t ea = ringb_alloc(ls, m, tag_get, rd32(ls, line + 28), blk + 28, rd32(ls, nxt), line, sp - 272);
    uint32_t chunk = fetched - hdr;
    wr32(ls, blk + 8, ea);
    uint32_t start = rd32(ls, line + 32);
    if (start == ea) wr8(ls, blk + 17, 1);
    wr32(ls, blk + 24, (rd32(ls, line + 48) + (ea - start)) | 2);
    uint32_t done = 0;
    uint32_t window = chunk;
    for (;;) {
        uint32_t len = window - done;
        uint32_t n = rd16(ls, params + 62);
        if (n != 0) {
            uint32_t buf = rd32(ls, blk + 40);
            uint32_t idx = buf + 16 * n;
            for (uint32_t k = 0; k < n; k++) {
                uint32_t h = quad_rd16(ls, idx + 2 * k);
                if (h == 0xffff) continue;
                uint32_t off = quad_rd32(ls, nxt + 4 * h + 24);
                uint32_t cnt = quad_rd16(ls, nxt + off);
                for (uint32_t j = 0; j < cnt; j++) {
                    uint32_t v = quad_rd16(ls, nxt + off + 2 + 2 * j);
                    if (16 * v - done < len) copy_quad(ls, data + 16 * (v - (done >> 4)), buf + 16 * k);
                }
            }
        }
        put_chunked(m, data, ea + done, len, tag_put, false);
        m.wait_tag(mask);
        if (sz56 <= fetched) break;
        uint32_t rem = sz56 - fetched;
        uint32_t next = chunk > rem ? rem : chunk;
        get_chunked(m, data, src + fetched, next, tag_get);
        m.wait_tag(mask);
        done += chunk;
        fetched += next;
        window = next + done;
    }
    m.putlluc(line, ring_line_ea(ls, params));
}

}
