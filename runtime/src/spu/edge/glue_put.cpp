#include "edge_glue.h"

namespace edge::glue {

namespace {

uint32_t mul16(uint32_t n, uint32_t stride) {
    uint32_t lo = (n & 0xffff) * (stride & 0xffff);
    uint32_t hi = ((stride >> 16) & 0xffff) * (n & 0xffff);
    return lo + (hi << 16);
}

void write_alloc_result(uint8_t* ls, uint32_t out, uint32_t ea, uint32_t io, uint32_t loc) {
    wr32(ls, out, ea);
    wr32(ls, out + 4, io);
    wr32(ls, out + 8, loc);
}

void advance_alloc(uint8_t* ls, uint32_t alloc, uint32_t ea, uint32_t io, uint32_t size) {
    wr32(ls, alloc, ea + size);
    wr32(ls, alloc + 4, io + size);
    ls[(alloc + 29) & kLsMask] = 1;
}

}

uint32_t hole_offset_check(uint32_t w1, uint32_t a, uint32_t b, uint32_t c) {
    (void)c;
    uint32_t s = ((w1 & 0xffff) << 2) - (a + b);
    return ((w1 >> 16) << 2) + s;
}

bool output_vertex_stride(const uint8_t* ls, uint32_t ctx, uint32_t fmt_id, uint32_t& stride) {
    (void)fmt_id;
    if (rd32(ls, ctx + 136) != 0) return false;
    uint32_t desc = rd32(ls, ctx + 116);
    stride = desc ? ls[(desc + 1) & kLsMask] : 0;
    return true;
}

bool output_sizes(const uint8_t* ls, uint32_t ctx, uint32_t num_indexes, uint32_t& size) {
    uint32_t stride;
    if (!output_vertex_stride(ls, ctx, ls[(ctx + 4) & kLsMask], stride)) return false;
    uint32_t idx = ((num_indexes + 7) & ~7u) * 2;
    uint32_t vert = (mul16(rd16(ls, ctx + 8), stride) + 15) & ~15u;
    size = (vert + idx + 127) & ~127u;
    return true;
}

void output_indexes(uint8_t* ls, Mfc& m, uint32_t ctx, uint32_t num_indexes, uint32_t alloc, uint32_t out) {
    if (num_indexes == 0) return;
    uint32_t size = ((num_indexes + 7) & ~7u) * 2;
    uint32_t ea = rd32(ls, alloc), io = rd32(ls, alloc + 4), loc = rd32(ls, alloc + 8);
    uint32_t lsa = ctx_index_list(ls, ctx);
    write_alloc_result(ls, out, ea, io, loc);
    put_chunked(m, lsa, ea, size, rd32(ls, ctx + 336), false);
    advance_alloc(ls, alloc, ea, io, size);
}

bool output_vertexes(uint8_t* ls, Mfc& m, uint32_t ctx, uint32_t alloc, uint32_t out) {
    if (ls[ctx & kLsMask] & 0x10) return true;
    uint32_t stride;
    if (!output_vertex_stride(ls, ctx, ls[(ctx + 4) & kLsMask], stride)) return false;
    uint32_t ea = rd32(ls, alloc), io = rd32(ls, alloc + 4), loc = rd32(ls, alloc + 8);
    uint32_t lsa = rd32(ls, ctx + 84);
    uint32_t size = (mul16(rd16(ls, ctx + 8), stride) + 15) & ~15u;
    write_alloc_result(ls, out, ea, io, loc);
    put_chunked(m, lsa, ea, size, rd32(ls, ctx + 336), false);
    advance_alloc(ls, alloc, ea, io, size);
    return true;
}

}
