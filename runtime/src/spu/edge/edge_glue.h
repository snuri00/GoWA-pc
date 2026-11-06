#pragma once

#include <cstdint>

namespace edge::glue {

constexpr uint32_t kLsMask = 0x3ffff;
constexpr uint32_t kCtxLs = 0x107a0;
constexpr uint32_t kParamsLs = 0x10650;
constexpr uint32_t kMatA = 0x10480;
constexpr uint32_t kMatB = 0x104c0;
constexpr uint32_t kRingLineLs = 0x10c00;
constexpr uint32_t kJobPtrLs = 0x10b00;

struct Mfc {
    virtual ~Mfc() = default;
    virtual void get(uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag) = 0;
    virtual void put(uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag, bool fence) = 0;
    virtual void getllar(uint32_t lsa, uint32_t ea) = 0;
    virtual bool putllc(uint32_t lsa, uint32_t ea) = 0;
    virtual void putlluc(uint32_t lsa, uint32_t ea) = 0;
    virtual void wait_tag(uint32_t mask) = 0;
    virtual uint32_t decrementer() = 0;
    virtual void getf(uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag) { get(lsa, ea, size, tag); }
};

inline uint32_t rd32(const uint8_t* ls, uint32_t a) {
    a &= kLsMask;
    return (uint32_t(ls[a]) << 24) | (uint32_t(ls[a + 1]) << 16) | (uint32_t(ls[a + 2]) << 8) | ls[a + 3];
}

inline uint16_t rd16(const uint8_t* ls, uint32_t a) {
    a &= kLsMask;
    return uint16_t((uint32_t(ls[a]) << 8) | ls[a + 1]);
}

inline void wr32(uint8_t* ls, uint32_t a, uint32_t v) {
    a &= kLsMask;
    ls[a] = uint8_t(v >> 24);
    ls[a + 1] = uint8_t(v >> 16);
    ls[a + 2] = uint8_t(v >> 8);
    ls[a + 3] = uint8_t(v);
}

inline void wr16(uint8_t* ls, uint32_t a, uint16_t v) {
    a &= kLsMask;
    ls[a] = uint8_t(v >> 8);
    ls[a + 1] = uint8_t(v);
}

uint32_t spu_ring_index(const uint8_t* ls);
uint32_t read_label(uint8_t* ls, Mfc& m, uint32_t tag, uint32_t label_ea, uint32_t sp);
uint32_t fence_free_space(uint8_t* ls, Mfc& m, uint32_t tag, uint32_t label_ea, uint32_t start, uint32_t end, uint32_t cur, uint32_t label, uint32_t store, uint32_t sp);
uint32_t ring_stall_check(uint8_t* ls, Mfc& m, uint32_t params, uint32_t tag, uint32_t buf, uint32_t sp);

uint32_t ring_free_space(const uint8_t* ls, uint32_t line);
uint32_t ring_alloc_try(uint8_t* ls, Mfc& m, uint32_t ctx, uint32_t line, uint32_t size, uint32_t retry, uint32_t result, uint32_t sp);
uint32_t shared_alloc(uint8_t* ls, Mfc& m, uint32_t info, uint32_t size, uint32_t result, uint32_t sp);
uint32_t allocate_output_space(uint8_t* ls, Mfc& m, uint32_t ctx, uint32_t info, uint32_t size, uint32_t result, uint32_t ring, uint32_t sp);

struct HoleState {
    uint32_t cur = 0;
    uint32_t end = 0;
    uint32_t base = 0;
    uint32_t hole_ea = 0;
    uint32_t w[8] = {};
};

uint32_t ctx_index_list(const uint8_t* ls, uint32_t ctx);
uint32_t ctx_scratch_ptr(const uint8_t* ls, uint32_t ctx);
void set_ctx_scratch_ptr(uint8_t* ls, uint32_t ctx, uint32_t v);
uint32_t ctx_position_table(const uint8_t* ls, uint32_t ctx);
uint32_t ctx_attr2_table(const uint8_t* ls, uint32_t ctx);
uint32_t ctx_attr3_table(const uint8_t* ls, uint32_t ctx);
uint32_t ctx_attr4_table(const uint8_t* ls, uint32_t ctx);
uint32_t ctx_uniform_table(const uint8_t* ls, uint32_t ctx, uint32_t slot);

uint32_t dma_chunk_size(uint32_t size);
uint32_t put_chunked(Mfc& m, uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag, bool fence);
uint32_t get_chunked(Mfc& m, uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag);

constexpr uint32_t kHoleBegin = 0;
constexpr uint32_t kHoleEnd = 4;
constexpr uint32_t kHoleCur = 8;
constexpr uint32_t kHoleCallback = 12;
constexpr uint32_t kSetArraysBss = 0x10b80;

using OverlayCallFn = bool (*)(void* user, uint8_t* ls, uint32_t entry, const uint32_t* r3_to_r10);

struct OverlayHook {
    OverlayCallFn call = nullptr;
    void* user = nullptr;
};

uint32_t hole_offset_check(uint32_t w1, uint32_t a, uint32_t b, uint32_t c);
bool output_vertex_stride(const uint8_t* ls, uint32_t ctx, uint32_t fmt_id, uint32_t& stride);
bool output_sizes(const uint8_t* ls, uint32_t ctx, uint32_t num_indexes, uint32_t& size);
void output_indexes(uint8_t* ls, Mfc& m, uint32_t ctx, uint32_t num_indexes, uint32_t alloc, uint32_t out);
bool output_vertexes(uint8_t* ls, Mfc& m, uint32_t ctx, uint32_t alloc, uint32_t out);

uint32_t hole_reserve(const uint8_t* ls, uint32_t state, uint32_t words);
bool set_fragment_program(uint8_t* ls, uint32_t state, uint32_t prog, uint32_t control, uint32_t tex2d, uint32_t centroid);
bool set_transform_constant(uint8_t* ls, uint32_t state, uint32_t slot, uint32_t src);
bool set_scissor(uint8_t* ls, uint32_t state, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
bool set_viewport(uint8_t* ls, uint32_t state, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t zmin, uint32_t zmax);
bool set_instance_state(uint8_t* ls, uint32_t inst, uint32_t state);
bool set_vertex_data_arrays(uint8_t* ls, uint32_t ctx, uint32_t state, uint32_t location, uint32_t io_offset, uint32_t entry, const OverlayHook& hook);
void begin_command_buffer_hole(uint8_t* ls, Mfc& m, uint32_t ctx, uint32_t state, uint32_t hole_ea, uint32_t allocs, uint32_t num_allocs);
uint32_t end_command_buffer_hole(uint8_t* ls, Mfc& m, uint32_t ctx, uint32_t state, uint32_t hole_ea, uint32_t allocs, uint32_t num_allocs);

uint32_t clear_quads(uint8_t* ls, uint32_t dst, uint32_t count);
uint32_t next_uniform_table(const uint8_t* ls, uint32_t ctx);
void tag_wait(Mfc& m, uint32_t mask);
uint32_t wait_label_outside(uint8_t* ls, Mfc& m, uint32_t tag, uint32_t label_ea, uint32_t lo1, uint32_t lo2, uint32_t hi1, uint32_t hi2, uint32_t sp);
uint32_t ringb_alloc(uint8_t* ls, Mfc& m, uint32_t tag, uint32_t label_ea, uint32_t out_ptr, uint32_t req, uint32_t line, uint32_t sp);
uint32_t ringb_reserve(uint8_t* ls, Mfc& m, uint32_t ctx, uint32_t blk, uint32_t params);
void ringb_commit(uint8_t* ls, Mfc& m, uint32_t ctx, uint32_t blk, uint32_t params, uint32_t sp);

bool softbody_prepare(uint8_t* ls, uint32_t blk, uint32_t limit, uint32_t disable);
bool softprep_9a60(uint8_t* ls, Mfc& m, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t sp, uint32_t& r3_out);
void asym_deform(uint8_t* ls, uint32_t out, uint32_t in, uint32_t count, uint32_t blk, const uint32_t scale[4]);


int32_t attr_map_find(const uint8_t* ls, uint32_t base, uint32_t id);
int32_t slot_find(const uint8_t* ls, uint32_t ctx, uint32_t id);
uint32_t slot_table(const uint8_t* ls, uint32_t ctx, uint32_t id);
uint32_t slot_set(uint8_t* ls, uint32_t ctx, uint32_t id, uint32_t value);
uint32_t slot_alloc(uint8_t* ls, uint32_t ctx, uint32_t id);

}
