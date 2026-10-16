#include "shader_compiler.h"

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include <mutex>

namespace gpu {

namespace {

std::once_flag g_init;

glslang_stage_t to_stage(ShaderStage s) {
    switch (s) {
    case ShaderStage::Vertex: return GLSLANG_STAGE_VERTEX;
    case ShaderStage::Fragment: return GLSLANG_STAGE_FRAGMENT;
    default: return GLSLANG_STAGE_COMPUTE;
    }
}

}

bool compile_glsl(ShaderStage stage, const std::string& source, std::vector<uint32_t>& spirv, std::string& log) {
    std::call_once(g_init, [] { glslang_initialize_process(); });
    glslang_input_t input{};
    input.language = GLSLANG_SOURCE_GLSL;
    input.stage = to_stage(stage);
    input.client = GLSLANG_CLIENT_VULKAN;
    input.client_version = GLSLANG_TARGET_VULKAN_1_3;
    input.target_language = GLSLANG_TARGET_SPV;
    input.target_language_version = GLSLANG_TARGET_SPV_1_6;
    input.code = source.c_str();
    input.default_version = 450;
    input.default_profile = GLSLANG_NO_PROFILE;
    input.messages = GLSLANG_MSG_DEFAULT_BIT;
    input.resource = glslang_default_resource();
    glslang_shader_t* shader = glslang_shader_create(&input);
    bool ok = glslang_shader_preprocess(shader, &input) && glslang_shader_parse(shader, &input);
    if (!ok) {
        log = std::string(glslang_shader_get_info_log(shader)) + glslang_shader_get_info_debug_log(shader);
        glslang_shader_delete(shader);
        return false;
    }
    glslang_program_t* program = glslang_program_create();
    glslang_program_add_shader(program, shader);
    ok = glslang_program_link(program, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT);
    if (!ok) {
        log = glslang_program_get_info_log(program);
    } else {
        glslang_program_SPIRV_generate(program, input.stage);
        size_t n = glslang_program_SPIRV_get_size(program);
        spirv.resize(n);
        glslang_program_SPIRV_get(program, spirv.data());
    }
    glslang_program_delete(program);
    glslang_shader_delete(shader);
    return ok;
}

}
