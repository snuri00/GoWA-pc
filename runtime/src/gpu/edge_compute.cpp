#include "edge_compute.h"

#include "../spu/edge/edge_gpu.h"
#include "runtime/log.h"
#include "shader_compiler.h"
#include "vk_context.h"

#include <cstring>
#include <string>
#include <vector>

namespace gpu::edge_compute {

namespace {

const char* kShader = R"(#version 450
#extension GL_ARB_gpu_shader_int64 : require
layout(local_size_x = 64) in;
layout(std430, binding = 0) readonly buffer In { uint w[]; } src;
layout(std430, binding = 1) writeonly buffer Out { vec4 v[]; } dst;
layout(push_constant) uniform P { uint count; } pc;

uint byte_at(uint b) { return (src.w[32u + (b >> 2)] >> ((b & 3u) * 8u)) & 0xFFu; }

float daz(float f) { return abs(f) < 1.17549435e-38 ? 0.0 : f; }
float down(float f) { return uintBitsToFloat(floatBitsToUint(f) - 1u); }

float rz1(double d) {
    if (d == 0.0LF) return 0.0;
    float f = float(d);
    if (abs(double(f)) > abs(d)) f = down(f);
    return daz(f);
}

float rz2(double s, double e) {
    if (s == 0.0LF) return rz1(e);
    float f = float(s);
    if (abs(double(f)) > abs(s)) f = down(f);
    else if (double(f) == s && e != 0.0LF && ((s > 0.0LF) != (e > 0.0LF))) f = down(f);
    return daz(f);
}

void two_sum(double a, double b, out double s, out double e) {
    precise double ss = a + b;
    precise double bb = ss - a;
    precise double ea = ss - bb;
    precise double e1 = a - ea;
    precise double e2 = b - bb;
    precise double ee = e1 + e2;
    s = ss;
    e = ee;
}

float fa_(float a, float b) {
    double s, e;
    two_sum(double(daz(a)), double(daz(b)), s, e);
    return rz2(s, e);
}

float fm_(float a, float b) {
    precise double p = double(daz(a)) * double(daz(b));
    return rz1(p);
}

float fma_(float a, float b, float c) {
    precise double p = double(daz(a)) * double(daz(b));
    double s, e;
    two_sum(p, double(daz(c)), s, e);
    return rz2(s, e);
}

float mat(uint r, uint c) { return uintBitsToFloat(src.w[16u + r * 4u + c]); }

void main() {
    uint v = gl_GlobalInvocationID.x;
    if (v >= pc.count) return;
    uint64_t win = 0ul;
    for (uint i = 0u; i < 8u; i++) win = (win << 8) | uint64_t(byte_at(12u * v + i));
    float p[4];
    uint pos = 0u;
    for (uint c = 0u; c < 3u; c++) {
        uint off = src.w[1u + c];
        uint width = src.w[4u + c] & 63u;
        uint frac = src.w[7u + c];
        uint field = width == 0u ? 0u : uint((win << pos) >> (64u - width));
        pos += width;
        uint sum = field + off;
        int ip = frac >= 32u ? (int(sum) >> 31) : (int(sum) >> frac);
        uint sh = (32u - frac) & 63u;
        uint fp = sh >= 32u ? 0u : (sum << sh);
        float a = rz1(double(ip));
        float b = rz1(double(fp) * 2.3283064365386962890625e-10LF);
        p[c] = fa_(a, b);
    }
    p[3] = 1.0;
    float o[4];
    for (uint i = 0u; i < 4u; i++) {
        float t = fm_(p[i], mat(i, i));
        t = fma_(p[(i + 1u) & 3u], mat(i, (i + 1u) & 3u), t);
        t = fma_(p[(i + 2u) & 3u], mat(i, (i + 2u) & 3u), t);
        o[i] = fma_(p[(i + 3u) & 3u], mat(i, (i + 3u) & 3u), t);
    }
    dst.v[v] = vec4(o[0], o[1], o[2], 1.0);
}
)";

struct State {
    bool tried = false;
    bool ok = false;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
} g;

struct Check {
    const float* gpu;
    const float* cpu;
    uint32_t count;
    uint32_t ea;
};

std::vector<Check> g_checks;
uint64_t g_frames = 0, g_dispatches = 0, g_vertices = 0, g_checked = 0, g_bad_vertices = 0, g_bad_draws = 0;

int env_mode() { return edge::gpu::mode(); }

bool init() {
    if (g.tried) return g.ok;
    g.tried = true;
    const VkCtx& c = vk();
    if (!c.features.shaderFloat64 || !c.features.shaderInt64) {
        LOG_WARN("edgegpu", "shaderFloat64/shaderInt64 not supported, Edge GPU path disabled");
        return false;
    }
    std::vector<uint32_t> spirv;
    std::string log;
    if (!compile_glsl(ShaderStage::Compute, kShader, spirv, log)) {
        LOG_ERROR("edgegpu", "compute shader failed: %s", log.c_str());
        return false;
    }
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = spirv.size() * 4;
    mi.pCode = spirv.data();
    VkShaderModule module;
    if (vkCreateShaderModule(c.device, &mi, nullptr, &module) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b[2]{};
    for (uint32_t i = 0; i < 2; i++) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dl.bindingCount = 2;
    dl.pBindings = b;
    if (vkCreateDescriptorSetLayout(c.device, &dl, nullptr, &g.set_layout) != VK_SUCCESS) return false;
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g.set_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(c.device, &pl, nullptr, &g.layout) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
    ci.layout = g.layout;
    VkResult r = vkCreateComputePipelines(c.device, VK_NULL_HANDLE, 1, &ci, nullptr, &g.pipeline);
    vkDestroyShaderModule(c.device, module, nullptr);
    if (r != VK_SUCCESS) return false;
    g.ok = true;
    LOG_INFO("edgegpu", "Edge vertex compute pass ready (mode %d)", env_mode());
    return true;
}

}

bool enabled() { return env_mode() == edge::gpu::kDefer || env_mode() == edge::gpu::kVerify; }

bool verify() { return env_mode() == edge::gpu::kVerify; }

bool expand(const edge::gpu::Record& r, RingAlloc& out) {
    if (!init()) return false;
    uint32_t stream_words = static_cast<uint32_t>((r.stream.size() + 3) / 4);
    VkDeviceSize in_size = VkDeviceSize(32 + stream_words) * 4;
    RingAlloc in;
    if (!ring_alloc(in_size, 16, in)) return false;
    if (!ring_alloc(VkDeviceSize(r.nv) * 16, 16, out)) return false;
    uint32_t hdr[32] = {};
    hdr[0] = r.nv;
    for (int c = 0; c < 3; c++) {
        hdr[1 + c] = r.off[c];
        hdr[4 + c] = r.wid[c];
        hdr[7 + c] = r.frac[c];
    }
    std::memcpy(hdr + 16, r.m, 64);
    std::memcpy(in.ptr, hdr, sizeof hdr);
    std::memset(in.ptr + sizeof hdr + r.stream.size(), 0, stream_words * 4 - r.stream.size());
    std::memcpy(in.ptr + sizeof hdr, r.stream.data(), r.stream.size());
    VkCommandBuffer cmd = pre_cmd();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.pipeline);
    VkDescriptorBufferInfo bi[2] = {{in.buffer, in.offset, in_size}, {out.buffer, out.offset, VkDeviceSize(r.nv) * 16}};
    VkWriteDescriptorSet wr[2]{};
    for (uint32_t i = 0; i < 2; i++) wr[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, i, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bi[i], nullptr};
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.layout, 0, 2, wr);
    uint32_t pcv[4] = {r.nv, 0, 0, 0};
    vkCmdPushConstants(cmd, g.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pcv);
    vkCmdDispatch(cmd, (r.nv + 63) / 64, 1, 1);
    g_dispatches++;
    g_vertices += r.nv;
    return true;
}

void add_check(const float* gpu, const float* cpu, uint32_t count, uint32_t ea) { g_checks.push_back({gpu, cpu, count, ea}); }

void frame_complete() {
    if (!env_mode()) return;
    for (const Check& c : g_checks) {
        uint32_t bad = 0;
        for (uint32_t i = 0; i < c.count * 4; i++) {
            if (std::memcmp(c.gpu + i, c.cpu + i, 4) != 0) {
                if (bad == 0 && g_bad_draws < 20) {
                    uint32_t a, b;
                    std::memcpy(&a, c.gpu + i, 4);
                    std::memcpy(&b, c.cpu + i, 4);
                    LOG_WARN("edgegpu", "mismatch ea 0x%08x vertex %u comp %u gpu 0x%08x cpu 0x%08x", c.ea, i / 4, i % 4, a, b);
                }
                bad++;
            }
        }
        g_checked += c.count;
        if (bad) {
            g_bad_draws++;
            g_bad_vertices += bad;
        }
    }
    g_checks.clear();
    edge::gpu::purge(20000);
    if (++g_frames % 300 == 0) {
        edge::gpu::Stats s = edge::gpu::stats();
        LOG_INFO("edgegpu", "spu deferred %llu confirmed %llu published %llu put skipped %llu dropped %llu materialized %llu/%llu/%llu/%llu/%llu/%llu/%llu | gpu dispatches %llu vertices %llu | verify checked %llu vertices, %llu bad components in %llu draws",
                 (unsigned long long)s.deferred, (unsigned long long)s.confirmed, (unsigned long long)s.published, (unsigned long long)s.put_skipped, (unsigned long long)s.dropped,
                 (unsigned long long)s.materialized[1], (unsigned long long)s.materialized[2], (unsigned long long)s.materialized[3], (unsigned long long)s.materialized[4],
                 (unsigned long long)s.materialized[5], (unsigned long long)s.materialized[6], (unsigned long long)s.materialized[7], (unsigned long long)g_dispatches,
                 (unsigned long long)g_vertices, (unsigned long long)g_checked, (unsigned long long)g_bad_vertices, (unsigned long long)g_bad_draws);
    }
}

}
