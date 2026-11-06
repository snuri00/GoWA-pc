#pragma once

#include "edge_glue.h"
#include "spu/context.h"

#include <cstdint>
#include <map>

using GluePortFn = bool (*)(uint8_t* ls, spu::Context& c, edge::glue::Mfc& m);

struct GluePort {
    uint32_t lsa = 0;
    const char* name = "";
    GluePortFn run = nullptr;
    bool has_r3 = false;
};

std::map<uint32_t, GluePort>& glue_ports();

struct GluePortReg {
    GluePortReg(uint32_t lsa, const char* name, GluePortFn run, bool has_r3) { glue_ports()[lsa] = GluePort{lsa, name, run, has_r3}; }
};
