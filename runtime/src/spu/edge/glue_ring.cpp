#include "edge_glue.h"

#include "spu_float.h"

namespace edge::glue {

namespace {

constexpr uint32_t kQuadMask = 0x3fff0;
constexpr uint32_t kRingBFraction = 0x3e199999;
constexpr uint32_t kRingAFraction = 0x3dcccccc;
constexpr int kBackoffReads = 2400;

inline uint32_t quad_word(const uint8_t* ls, uint32_t a, uint32_t i) { return rd32(ls, (a & kQuadMask) + 4 * i); }

inline uint32_t label_buffer(uint32_t sp) { return ((sp - 129) & ~127u) & kLsMask; }

inline uint32_t fraction_of(uint32_t size, uint32_t fraction) {
    float f = spuf::fm(spuf::fl(spuf::cuflt(size, 155)), spuf::fl(fraction));
    return spuf::cfltu(spuf::bits(f), 173);
}

inline uint32_t fence_free(uint32_t start, uint32_t end, uint32_t cur, uint32_t label) {
    if (label > cur) return label - cur;
    return (end - cur) + (label - start);
}

}

uint32_t spu_ring_index(const uint8_t* ls) { return rd32(ls, 0x1c8); }

uint32_t read_label(uint8_t* ls, Mfc& m, uint32_t tag, uint32_t label_ea, uint32_t sp) {
    uint32_t lsa = ((sp - 16) | (label_ea & 15)) & kLsMask;
    m.get(lsa, label_ea, 4, tag);
    m.wait_tag(1u << (tag & 31));
    return rd32(ls, lsa);
}

uint32_t fence_free_space(uint8_t* ls, Mfc& m, uint32_t tag, uint32_t label_ea, uint32_t start, uint32_t end, uint32_t cur, uint32_t label, uint32_t store, uint32_t sp) {
    if (store != 0) {
        label = read_label(ls, m, tag, label_ea, sp - 96);
        wr32(ls, store & ~3u, label);
    }
    return fence_free(start, end, cur, label);
}

uint32_t ring_stall_check(uint8_t* ls, Mfc& m, uint32_t params, uint32_t tag, uint32_t buf, uint32_t sp) {
    uint32_t line_ea = quad_word(ls, params, 0) + 0x80 + (spu_ring_index(ls) << 7);
    m.getllar(buf, line_ea);
    spuf::CsrGuard guard;
    uint32_t w[12];
    for (int i = 0; i < 12; i++) w[i] = rd32(ls, buf + 4 * i);
    uint32_t thr_b = fraction_of(w[9], kRingBFraction);
    uint32_t thr_a = fraction_of(w[2] - w[0], kRingAFraction);
    uint32_t sp1 = sp - 224;
    uint32_t free_b = fence_free(w[8], w[8] + w[9], w[11], w[10]);
    bool need_b = thr_b > free_b;
    bool need_a = true;
    bool a_ok = false;
    bool b_ok = !need_b;
    if (w[6] != 0 && !(thr_a > fence_free(w[0], w[2], w[3], w[6]))) {
        if (!need_b) return 0;
        a_ok = true;
        need_a = false;
    }
    if (need_b) {
        uint32_t fb = fence_free_space(ls, m, tag, w[7], w[8], w[8] + w[9], w[11], w[10], buf + 40, sp1);
        b_ok = !(thr_b > fb);
    }
    if (need_a) {
        uint32_t fa = fence_free_space(ls, m, tag, w[5], w[0], w[2], w[3], w[6], buf + 24, sp1);
        a_ok = !(thr_a > fa);
    }
    if (!b_ok || !a_ok) return 1;
    m.putlluc(buf, line_ea);
    return 0;
}

uint32_t ring_free_space(const uint8_t* ls, uint32_t line) {
    uint32_t label = rd32(ls, line + 24);
    if (label == 0) return 0;
    uint32_t cur = rd32(ls, line + 12);
    if (cur > label) return (rd32(ls, line + 8) - cur) + (label - rd32(ls, line));
    return label - cur;
}

uint32_t ring_alloc_try(uint8_t* ls, Mfc& m, uint32_t ctx, uint32_t line, uint32_t size, uint32_t retry, uint32_t result, uint32_t sp) {
    uint32_t start = rd32(ls, line);
    uint32_t end = rd32(ls, line + 8);
    uint32_t cur = rd32(ls, line + 12);
    uint32_t label_ea = rd32(ls, line + 20);
    if (size + size > end - start) return 0;
    uint32_t lsa = (label_buffer(sp) + (label_ea & 127)) & kLsMask;
    uint32_t new_cur = cur + size;
    uint32_t wrap_cur = start + size;
    uint32_t alloc = cur;
    uint32_t alloc_end = new_cur;
    bool need_read = rd32(ls, line + 24) == 0;
    for (;;) {
        if (need_read || (ls[((ctx + 400) & kQuadMask) + 2] & 0x80)) {
            uint32_t tag = quad_word(ls, ctx + 336, 1);
            m.get(lsa, label_ea, 4, tag);
            m.wait_tag(1u << (tag & 31));
            wr32(ls, line + 24, rd32(ls, lsa));
        }
        uint32_t label = rd32(ls, line + 24);
        bool fits = !(new_cur > end);
        bool room = fits || label > wrap_cur;
        bool inside = label > cur && !(label > new_cur);
        if (room && !inside) {
            if (!fits) {
                alloc = start;
                alloc_end = wrap_cur;
                ls[(result + 28) & kLsMask] = 1;
            }
            break;
        }
        if (retry == 0 && need_read) return 0;
        if (!need_read) {
            need_read = true;
            continue;
        }
        for (int i = 0; i < kBackoffReads; i++) m.decrementer();
    }
    wr32(ls, line + 12, alloc_end);
    wr32(ls, result, alloc);
    wr32(ls, result + 4, rd32(ls, line + 4) + (alloc - start));
    wr32(ls, result + 8, rd32(ls, line + 16));
    return 1;
}

uint32_t shared_alloc(uint8_t* ls, Mfc& m, uint32_t info, uint32_t size, uint32_t result, uint32_t sp) {
    uint32_t buf = label_buffer(sp);
    uint32_t got;
    do {
        m.getllar(buf, info);
        uint32_t cur = rd32(ls, buf + 12);
        uint32_t new_cur = cur + size;
        if (new_cur > rd32(ls, buf + 8)) {
            got = 0;
            wr32(ls, buf + 20, rd32(ls, buf + 20) + size);
        } else {
            got = cur;
            wr32(ls, buf + 12, new_cur);
            wr32(ls, result + 8, rd32(ls, buf + 16));
        }
    } while (!m.putllc(buf, info));
    wr32(ls, result, got);
    wr32(ls, result + 4, rd32(ls, buf + 4) + (got - rd32(ls, buf)));
    return got != 0 ? 1 : 0;
}

uint32_t allocate_output_space(uint8_t* ls, Mfc& m, uint32_t ctx, uint32_t info, uint32_t size, uint32_t result, uint32_t ring, uint32_t sp) {
    for (uint32_t i = 28; i < 31; i++) ls[(result + i) & kLsMask] = 0;
    if (info & 2) {
        uint32_t base = info & ~3u;
        wr32(ls, result + 16, 0);
        wr32(ls, result, base);
        wr32(ls, result + 4, 0xffff);
        wr32(ls, result + 8, info & 1);
        wr32(ls, result + 12, base + size);
        return 1;
    }
    uint32_t rsize = (size + 127) & ~127u;
    uint32_t line_ea = info + 0x80 + (ring << 7);
    uint32_t sp1 = sp - 224;
    m.getllar(kRingLineLs, line_ea);
    uint32_t free0 = ring_free_space(ls, kRingLineLs);
    wr32(ls, result + 20, free0);
    wr32(ls, result + 24, free0);
    uint32_t got;
    bool from_ring = false;
    if (rd32(ls, kRingLineLs + 8) == rd32(ls, kRingLineLs)) {
        got = shared_alloc(ls, m, info, rsize, result, sp1);
    } else {
        got = ring_alloc_try(ls, m, ctx, kRingLineLs, rsize, 0, result, sp1);
        if (got != 0) {
            from_ring = true;
        } else {
            got = shared_alloc(ls, m, info, rsize, result, sp1);
            if (got == 0) {
                got = ring_alloc_try(ls, m, ctx, kRingLineLs, rsize, 1, result, sp1);
                ls[(result + 30) & kLsMask] = 1;
                from_ring = true;
            }
        }
    }
    if (from_ring) {
        wr32(ls, result + 24, ring_free_space(ls, kRingLineLs));
        m.putlluc(kRingLineLs, line_ea);
    }
    if (got == 0) return 0;
    wr32(ls, result + 16, from_ring ? rd32(ls, kRingLineLs + 20) : 0);
    wr32(ls, result + 12, rd32(ls, result) + rsize);
    return 1;
}

}
