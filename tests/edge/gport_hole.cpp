#include "glue_reg.h"
#include "spu/context.h"

namespace {

using edge::glue::Mfc;

uint32_t arg(const spu::Context& c, int r) { return c.gpr[r].u32[3]; }

void ret(spu::Context& c, uint32_t v) { c.gpr[3].u32[3] = v; }

void save_lr(uint8_t* ls, const spu::Context& c) {
    uint32_t a = (c.gpr[1].u32[3] + 16) & edge::glue::kLsMask & ~15u;
    for (int i = 0; i < 4; i++) edge::glue::wr32(ls, a + 4 * i, c.gpr[0].u32[3 - i]);
}

class QuietHost : public spu::Host {
public:
    bool failed = false;
    uint32_t read_channel(uint32_t) override {
        failed = true;
        return 0;
    }
    void write_channel(uint32_t, uint32_t) override { failed = true; }
    uint32_t channel_count(uint32_t) override {
        failed = true;
        return 0;
    }
    bool stop_and_signal(uint32_t) override {
        failed = true;
        return false;
    }
    void halt(uint32_t, uint32_t) override { failed = true; }
};

bool run_overlay(void* user, uint8_t* ls, uint32_t entry, const uint32_t* regs) {
    spu::Context& c = *static_cast<spu::Context*>(user);
    c.ls = ls;
    for (int i = 0; i < 8; i++) c.gpr[3 + i].u32[3] = regs[i];
    c.gpr[4].u32[0] = c.gpr[4].u32[1] = c.gpr[4].u32[2] = 0;
    c.gpr[8].u32[0] = c.gpr[8].u32[1] = c.gpr[8].u32[2] = 0;
    c.pc = entry;
    uint32_t lr = c.gpr[0].u32[3], sp = c.gpr[1].u32[3];
    QuietHost h;
    for (uint64_t n = 0; n < (1ull << 24); n++) {
        if (!spu::step(c, h) || h.failed) return false;
        if (c.pc == lr && c.gpr[1].u32[3] == sp) return true;
    }
    return false;
}

bool p_aee0(uint8_t*, spu::Context& c, Mfc&) {
    ret(c, edge::glue::hole_offset_check(c.gpr[3].u32[2], arg(c, 4), arg(c, 5), arg(c, 6)));
    return true;
}

bool p_d118(uint8_t* ls, spu::Context& c, Mfc&) {
    uint32_t stride;
    if (!edge::glue::output_vertex_stride(ls, arg(c, 3), arg(c, 4), stride)) return false;
    ret(c, stride);
    return true;
}

bool p_bb38(uint8_t* ls, spu::Context& c, Mfc&) {
    save_lr(ls, c);
    uint32_t size;
    if (!edge::glue::output_sizes(ls, arg(c, 3), arg(c, 4), size)) return false;
    ret(c, size);
    return true;
}

bool p_c340(uint8_t* ls, spu::Context& c, Mfc& m) {
    save_lr(ls, c);
    edge::glue::output_indexes(ls, m, arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 6));
    return true;
}

bool p_c490(uint8_t* ls, spu::Context& c, Mfc& m) {
    save_lr(ls, c);
    return edge::glue::output_vertexes(ls, m, arg(c, 3), arg(c, 4), arg(c, 5));
}

bool p_aff0(uint8_t* ls, spu::Context& c, Mfc&) {
    save_lr(ls, c);
    if (edge::glue::rd32(ls, arg(c, 3) + edge::glue::kHoleCallback) != 0) return false;
    ret(c, edge::glue::hole_reserve(ls, arg(c, 3), arg(c, 4)));
    return true;
}

bool p_b028(uint8_t* ls, spu::Context& c, Mfc&) {
    save_lr(ls, c);
    return edge::glue::set_fragment_program(ls, arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 6), arg(c, 7));
}

bool p_b1a8(uint8_t* ls, spu::Context& c, Mfc&) {
    save_lr(ls, c);
    return edge::glue::set_transform_constant(ls, arg(c, 3), arg(c, 4), arg(c, 5));
}

bool p_b2d0(uint8_t* ls, spu::Context& c, Mfc&) {
    save_lr(ls, c);
    return edge::glue::set_scissor(ls, arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 6), arg(c, 7));
}

bool p_b3d0(uint8_t* ls, spu::Context& c, Mfc&) {
    save_lr(ls, c);
    return edge::glue::set_viewport(ls, arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 6), arg(c, 7), arg(c, 8), arg(c, 9));
}

bool p_84c0(uint8_t* ls, spu::Context& c, Mfc&) {
    save_lr(ls, c);
    return edge::glue::set_instance_state(ls, arg(c, 3), arg(c, 4));
}

bool p_5d68(uint8_t* ls, spu::Context& c, Mfc&) {
    edge::glue::OverlayHook hook{run_overlay, &c};
    return edge::glue::set_vertex_data_arrays(ls, arg(c, 3), arg(c, 5), arg(c, 6), arg(c, 7), arg(c, 8), hook);
}

bool p_c9a8(uint8_t* ls, spu::Context& c, Mfc& m) {
    save_lr(ls, c);
    edge::glue::begin_command_buffer_hole(ls, m, arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 6), arg(c, 7));
    return true;
}

bool p_cc98(uint8_t* ls, spu::Context& c, Mfc& m) {
    ret(c, edge::glue::end_command_buffer_hole(ls, m, arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 6), arg(c, 7)));
    return true;
}

GluePortReg r_aee0(0x0aee0, "hole_offset_check", p_aee0, true);
GluePortReg r_d118(0x0d118, "output_vertex_stride", p_d118, true);
GluePortReg r_bb38(0x0bb38, "output_sizes", p_bb38, true);
GluePortReg r_c340(0x0c340, "output_indexes", p_c340, false);
GluePortReg r_c490(0x0c490, "output_vertexes", p_c490, false);
GluePortReg r_aff0(0x0aff0, "hole_reserve", p_aff0, true);
GluePortReg r_b028(0x0b028, "set_fragment_program", p_b028, false);
GluePortReg r_b1a8(0x0b1a8, "set_transform_constant", p_b1a8, false);
GluePortReg r_b2d0(0x0b2d0, "set_scissor", p_b2d0, false);
GluePortReg r_b3d0(0x0b3d0, "set_viewport", p_b3d0, false);
GluePortReg r_84c0(0x084c0, "set_instance_state", p_84c0, false);
GluePortReg r_5d68(0x05d68, "set_vertex_data_arrays", p_5d68, false);
GluePortReg r_c9a8(0x0c9a8, "begin_command_buffer_hole", p_c9a8, false);
GluePortReg r_cc98(0x0cc98, "end_command_buffer_hole", p_cc98, true);

}
