#include "edge_glue.h"

namespace edge::glue {

namespace {

uint32_t quad_word(const uint8_t* ls, uint32_t a, uint32_t byte_off) {
    uint32_t base = a & 0x3fff0;
    uint32_t v = 0;
    for (uint32_t i = 0; i < 4; i++) v = (v << 8) | ls[base + ((byte_off + i) & 15)];
    return v;
}

}

uint32_t ctx_index_list(const uint8_t* ls, uint32_t ctx) { return quad_word(ls, ctx + 80, 0); }

uint32_t ctx_scratch_ptr(const uint8_t* ls, uint32_t ctx) { return quad_word(ls, ctx + 80, 8); }

void set_ctx_scratch_ptr(uint8_t* ls, uint32_t ctx, uint32_t v) { wr32(ls, ((ctx + 80) & 0x3fff0) + 8, v); }

uint32_t ctx_position_table(const uint8_t* ls, uint32_t ctx) { return quad_word(ls, ctx + 80, 12); }

uint32_t ctx_attr2_table(const uint8_t* ls, uint32_t ctx) { return quad_word(ls, ctx + 96, 0); }

uint32_t ctx_attr3_table(const uint8_t* ls, uint32_t ctx) { return quad_word(ls, ctx + 96, 4); }

uint32_t ctx_attr4_table(const uint8_t* ls, uint32_t ctx) { return quad_word(ls, ctx + 96, 8); }

uint32_t ctx_uniform_table(const uint8_t* ls, uint32_t ctx, uint32_t slot) {
    uint32_t a = ctx + slot * 4;
    return quad_word(ls, a + 16, a & 15);
}

uint32_t dma_chunk_size(uint32_t size) {
    if (size > 0x3fff) return 0x4000;
    if (size > 15) return size & ~15u;
    if (size > 7) return 8;
    if (size > 3) return 4;
    if (size > 1) return 2;
    return 1;
}

uint32_t put_chunked(Mfc& m, uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag, bool fence) {
    while (size) {
        uint32_t n = dma_chunk_size(size);
        m.put(lsa, ea, n, tag, fence);
        lsa += n;
        ea += n;
        size -= n;
    }
    return lsa;
}

uint32_t get_chunked(Mfc& m, uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag) {
    while (size) {
        uint32_t n = dma_chunk_size(size);
        m.get(lsa, ea, n, tag);
        lsa += n;
        ea += n;
        size -= n;
    }
    return lsa;
}

}
