#include "glue_reg.h"

namespace {

using edge::glue::Mfc;

uint32_t arg(const spu::Context& c, int r) { return c.gpr[r].u32[3]; }

void ret(spu::Context& c, uint32_t v) { c.gpr[3].u32[3] = v; }

void save_link(uint8_t* ls, const spu::Context& c) {
    uint32_t sp = (c.gpr[1].u32[3] + 16) & 0x3fff0;
    for (int i = 0; i < 4; i++) edge::glue::wr32(ls, sp + 4 * i, c.gpr[0].u32[3 - i]);
}

bool p_5c88(uint8_t* ls, spu::Context& c, Mfc&) {
    ret(c, uint32_t(edge::glue::attr_map_find(ls, arg(c, 3), arg(c, 4))));
    return true;
}

bool p_be88(uint8_t* ls, spu::Context& c, Mfc&) {
    ret(c, uint32_t(edge::glue::slot_find(ls, arg(c, 3), arg(c, 4))));
    return true;
}

bool p_bee0(uint8_t* ls, spu::Context& c, Mfc&) {
    save_link(ls, c);
    ret(c, edge::glue::slot_table(ls, arg(c, 3), arg(c, 4)));
    return true;
}

bool p_bf38(uint8_t* ls, spu::Context& c, Mfc&) {
    save_link(ls, c);
    ret(c, edge::glue::slot_set(ls, arg(c, 3), arg(c, 4), arg(c, 5)));
    return true;
}

bool p_bfa8(uint8_t* ls, spu::Context& c, Mfc&) {
    save_link(ls, c);
    ret(c, edge::glue::slot_alloc(ls, arg(c, 3), arg(c, 4)));
    return true;
}

GluePortReg r_5c88(0x05c88, "attr_map_find", p_5c88, true);
GluePortReg r_be88(0x0be88, "slot_find", p_be88, true);
GluePortReg r_bee0(0x0bee0, "slot_table", p_bee0, true);
GluePortReg r_bf38(0x0bf38, "slot_set", p_bf38, true);
GluePortReg r_bfa8(0x0bfa8, "slot_alloc", p_bfa8, true);

}
