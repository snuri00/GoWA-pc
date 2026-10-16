#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gpu {

enum class ShaderStage { Vertex, Fragment, Compute };

bool compile_glsl(ShaderStage stage, const std::string& source, std::vector<uint32_t>& spirv, std::string& log);

}
