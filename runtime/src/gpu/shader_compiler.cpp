#include "shader_compiler.h"

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include <cstdio>
#include <filesystem>
#include <mutex>

namespace gpu {

namespace {

std::once_flag g_init;

uint64_t source_key(ShaderStage stage, const std::string& source) {
    uint64_t h = 1469598103934665603ull ^ (uint64_t(stage) + 1);
    for (unsigned char ch : source) {
        h ^= ch;
        h *= 1099511628211ull;
    }
    return h;
}

std::string cache_path(uint64_t key) {
    static const bool ready = [] {
        std::error_code ec;
        std::filesystem::create_directories("shader_cache", ec);
        return true;
    }();
    (void)ready;
    char name[48];
    std::snprintf(name, sizeof name, "shader_cache/%016llx.spv", static_cast<unsigned long long>(key));
    return name;
}

bool load_cached(const std::string& path, std::vector<uint32_t>& spirv) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    bool ok = size > 20 && (size % 4) == 0;
    if (ok) {
        spirv.resize(size_t(size) / 4);
        ok = std::fread(spirv.data(), 1, size_t(size), f) == size_t(size) && spirv[0] == 0x07230203u;
    }
    std::fclose(f);
    return ok;
}

void store_cached(const std::string& path, const std::vector<uint32_t>& spirv) {
    std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    bool ok = std::fwrite(spirv.data(), 4, spirv.size(), f) == spirv.size();
    std::fclose(f);
    std::error_code ec;
    if (ok) std::filesystem::rename(tmp, path, ec);
    else std::filesystem::remove(tmp, ec);
}

glslang_stage_t to_stage(ShaderStage s) {
    switch (s) {
    case ShaderStage::Vertex: return GLSLANG_STAGE_VERTEX;
    case ShaderStage::Fragment: return GLSLANG_STAGE_FRAGMENT;
    default: return GLSLANG_STAGE_COMPUTE;
    }
}

}

bool compile_glsl(ShaderStage stage, const std::string& source, std::vector<uint32_t>& spirv, std::string& log) {
    std::string path = cache_path(source_key(stage, source));
    if (load_cached(path, spirv)) return true;
    bool ok = compile_glsl_uncached(stage, source, spirv, log);
    if (ok) store_cached(path, spirv);
    return ok;
}

bool compile_glsl_uncached(ShaderStage stage, const std::string& source, std::vector<uint32_t>& spirv, std::string& log) {
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
