#include "glue_reg.h"

namespace {

using edge::glue::Mfc;

uint32_t arg(const spu::Context& c, int r) { return c.gpr[r].u32[3]; }

void ret(spu::Context& c, uint32_t v) { c.gpr[3].u32[3] = v; }

bool p_b5f0(uint8_t* ls, spu::Context& c, Mfc&) {
    ret(c, edge::glue::ctx_scratch_ptr(ls, arg(c, 3)));
    return true;
}

bool p_b600(uint8_t* ls, spu::Context& c, Mfc&) {
    if (arg(c, 1) & 15) return false;
    edge::glue::set_ctx_scratch_ptr(ls, arg(c, 3), arg(c, 4));
    return true;
}

bool p_b618(uint8_t* ls, spu::Context& c, Mfc&) {
    ret(c, edge::glue::ctx_uniform_table(ls, arg(c, 3), arg(c, 4)));
    return true;
}

bool p_b630(uint8_t*, spu::Context&, Mfc&) { return true; }

bool p_b638(uint8_t* ls, spu::Context& c, Mfc&) {
    ret(c, edge::glue::ctx_position_table(ls, arg(c, 3)));
    return true;
}

bool p_b648(uint8_t* ls, spu::Context& c, Mfc&) {
    ret(c, edge::glue::ctx_attr2_table(ls, arg(c, 3)));
    return true;
}

bool p_b650(uint8_t* ls, spu::Context& c, Mfc&) {
    ret(c, edge::glue::ctx_attr3_table(ls, arg(c, 3)));
    return true;
}

bool p_b660(uint8_t* ls, spu::Context& c, Mfc&) {
    ret(c, edge::glue::ctx_attr4_table(ls, arg(c, 3)));
    return true;
}

bool p_b670(uint8_t* ls, spu::Context& c, Mfc&) {
    ret(c, edge::glue::ctx_index_list(ls, arg(c, 3)));
    return true;
}

bool p_af38(uint8_t*, spu::Context& c, Mfc& m) {
    uint32_t lsa = arg(c, 3), eah = c.gpr[4].u32[3], ea = c.gpr[4].u32[2], size = arg(c, 5), tag = arg(c, 6), cmd = arg(c, 7);
    if (eah) return false;
    uint32_t end;
    if (cmd == 0x20 || cmd == 0x22)
        end = edge::glue::put_chunked(m, lsa, ea, size, tag, cmd == 0x22);
    else if (cmd == 0x40)
        end = edge::glue::get_chunked(m, lsa, ea, size, tag);
    else
        return false;
    ret(c, end);
    return true;
}

GluePortReg r_b5f0(0x0b5f0, "ctx_scratch_ptr", p_b5f0, true);
GluePortReg r_b600(0x0b600, "set_ctx_scratch_ptr", p_b600, false);
GluePortReg r_b618(0x0b618, "ctx_uniform_table", p_b618, true);
GluePortReg r_b630(0x0b630, "nop", p_b630, false);
GluePortReg r_b638(0x0b638, "ctx_position_table", p_b638, true);
GluePortReg r_b648(0x0b648, "ctx_attr2_table", p_b648, true);
GluePortReg r_b650(0x0b650, "ctx_attr3_table", p_b650, true);
GluePortReg r_b660(0x0b660, "ctx_attr4_table", p_b660, true);
GluePortReg r_b670(0x0b670, "ctx_index_list", p_b670, true);
GluePortReg r_af38(0x0af38, "dma_chunked", p_af38, true);

}
