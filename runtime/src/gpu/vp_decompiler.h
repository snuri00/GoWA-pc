#pragma once

#include <cstdint>
#include <string>

namespace gpu {

struct VertexProgramInfo {
    std::string glsl;
    uint32_t input_mask = 0;
    uint32_t texture_mask = 0;
    bool valid = false;
};

VertexProgramInfo decompile_vertex_program(const uint32_t* ucode, uint32_t start_slot);

}
