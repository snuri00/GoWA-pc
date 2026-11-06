#include "glue_reg.h"

namespace {

using edge::glue::Mfc;

uint32_t arg(const spu::Context& c, int r) { return c.gpr[r].u32[3]; }

void ret(spu::Context& c, uint32_t v) { c.gpr[3].u32[3] = v; }

void save_link(uint8_t* ls, const spu::Context& c) {
    uint32_t a = (arg(c, 1) + 16) & 0x3fff0;
    for (int i = 0; i < 4; i++) edge::glue::wr32(ls, a + 4 * i, c.gpr[0].u32[3 - i]);
}

bool p_f770(uint8_t* ls, spu::Context& c, Mfc&) {
    ret(c, edge::glue::spu_ring_index(ls));
    return true;
}

bool p_4030(uint8_t* ls, spu::Context& c, Mfc& m) {
    ret(c, edge::glue::read_label(ls, m, arg(c, 3), arg(c, 4), arg(c, 1)));
    return true;
}

bool p_4110(uint8_t* ls, spu::Context& c, Mfc& m) {
    save_link(ls, c);
    ret(c, edge::glue::fence_free_space(ls, m, arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 6), arg(c, 7), arg(c, 8), arg(c, 9), arg(c, 1)));
    return true;
}

bool p_41a0(uint8_t* ls, spu::Context& c, Mfc& m) {
    save_link(ls, c);
    ret(c, edge::glue::ring_stall_check(ls, m, arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 1)));
    return true;
}

bool p_b678(uint8_t* ls, spu::Context& c, Mfc&) {
    ret(c, edge::glue::ring_free_space(ls, arg(c, 3)));
    return true;
}

bool p_b700(uint8_t* ls, spu::Context& c, Mfc& m) {
    ret(c, edge::glue::ring_alloc_try(ls, m, arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 6), arg(c, 7), arg(c, 1)));
    return true;
}

bool p_b9d0(uint8_t* ls, spu::Context& c, Mfc& m) {
    ret(c, edge::glue::shared_alloc(ls, m, arg(c, 4), arg(c, 5), arg(c, 6), arg(c, 1)));
    return true;
}

bool p_c628(uint8_t* ls, spu::Context& c, Mfc& m) {
    save_link(ls, c);
    ret(c, edge::glue::allocate_output_space(ls, m, arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 6), arg(c, 7), arg(c, 1)));
    return true;
}

GluePortReg r_f770(0x0f770, "spu_ring_index", p_f770, true);
GluePortReg r_4030(0x04030, "read_label", p_4030, true);
GluePortReg r_4110(0x04110, "fence_free_space", p_4110, true);
GluePortReg r_41a0(0x041a0, "ring_stall_check", p_41a0, true);
GluePortReg r_b678(0x0b678, "ring_free_space", p_b678, true);
GluePortReg r_b700(0x0b700, "ring_alloc_try", p_b700, true);
GluePortReg r_b9d0(0x0b9d0, "shared_alloc", p_b9d0, true);
GluePortReg r_c628(0x0c628, "allocate_output_space", p_c628, true);

}
