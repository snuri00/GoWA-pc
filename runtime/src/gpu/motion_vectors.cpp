#include "motion_vectors.h"
#include "dlssg.h"
#include "dlssnr.h"

#include "runtime/log.h"
#include "shader_compiler.h"

#include <immintrin.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace gpu::mv {

namespace {

const char* kShader = R"(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D vel;
layout(set = 0, binding = 1) uniform sampler2D dep;
layout(set = 0, binding = 2, rgba16f) uniform writeonly image2D cam_img;
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D game_img;
layout(set = 0, binding = 4, rg16f) uniform writeonly image2D out_img;
layout(push_constant) uniform P { vec4 ix; vec4 iy; vec4 iz; vec4 iw; vec4 px; vec4 py; vec4 pz; vec4 pw; vec4 size; } pc;
vec2 reproject(vec2 ndc, float d) {
    vec4 p = pc.ix * ndc.x + pc.iy * ndc.y + pc.iz * (2.0 * d - 1.0) + pc.iw;
    p /= p.w;
    vec4 q = pc.px * p.x + pc.py * p.y + pc.pz * p.z + pc.pw;
    return q.xy / q.w - ndc;
}
void main() {
    ivec2 c = ivec2(gl_GlobalInvocationID.xy);
    if (c.x >= int(pc.size.x) || c.y >= int(pc.size.y)) return;
    vec2 uv = (vec2(c) + 0.5) / pc.size.xy;
    float d = textureLod(dep, uv, 0.0).r;
    vec2 a = reproject(vec2(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0), d);
    vec2 b = reproject(vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0), d);
    vec4 g = textureLod(vel, uv, 0.0);
    vec2 gv = (g.rg * 2.0 - 1.0) * pc.size.w;
    imageStore(cam_img, c, vec4(a, b));
    imageStore(game_img, c, vec4(gv, g.b, d));
    vec2 flip = pc.size.z > 0.5 ? vec2(1.0, -1.0) : vec2(1.0);
    vec2 v = (g.b > 0.5 ? (pc.size.z > 0.5 ? b : a) : gv) * flip;
    imageStore(out_img, c, vec4(v * 0.5 * pc.size.xy, 0.0, 0.0));
}
)";

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

struct State {
    bool tried = false;
    bool ok = false;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    Image cam, game, out;
    uint32_t w = 0, h = 0;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    uint8_t* mapped = nullptr;
    VkDeviceSize staging_size = 0;
    float consts[32] = {};
    bool have_camera = false;
    uint32_t velocity_offset = 0;
    uint32_t depth_offset = 0;
    bool pending = false;
    double pending_time = 0.0;
    uint32_t dumps = 0;
    uint64_t runs = 0;
    VkImageLayout out_layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

State g;

double env_double(const char* name, double def) {
    const char* e = std::getenv(name);
    return e ? std::atof(e) : def;
}

bool create_image(Image& img, VkFormat format, uint32_t w, uint32_t h) {
    const VkCtx& c = vk();
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(c.device, &ii, nullptr, &img.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(c.device, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(c.device, &ai, nullptr, &img.memory) != VK_SUCCESS) return false;
    vkBindImageMemory(c.device, img.image, img.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vkCreateImageView(c.device, &vi, nullptr, &img.view) == VK_SUCCESS;
}

void destroy_image(Image& img) {
    const VkCtx& c = vk();
    if (img.view) vkDestroyImageView(c.device, img.view, nullptr);
    if (img.image) vkDestroyImage(c.device, img.image, nullptr);
    if (img.memory) vkFreeMemory(c.device, img.memory, nullptr);
    img = {};
}

bool init_pipeline() {
    if (g.tried) return g.ok;
    g.tried = true;
    const VkCtx& c = vk();
    std::vector<uint32_t> spirv;
    std::string log;
    if (!compile_glsl(ShaderStage::Compute, kShader, spirv, log)) {
        LOG_ERROR("mv", "compute shader failed: %s", log.c_str());
        return false;
    }
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = spirv.size() * 4;
    mi.pCode = spirv.data();
    VkShaderModule module;
    if (vkCreateShaderModule(c.device, &mi, nullptr, &module) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b[5]{};
    for (uint32_t i = 0; i < 5; i++) {
        b[i].binding = i;
        b[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dl.bindingCount = 5;
    dl.pBindings = b;
    if (vkCreateDescriptorSetLayout(c.device, &dl, nullptr, &g.set_layout) != VK_SUCCESS) return false;
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 144};
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
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    if (vkCreateSampler(c.device, &si, nullptr, &g.sampler) != VK_SUCCESS) return false;
    g.ok = true;
    LOG_INFO("mv", "motion vector debug pass ready");
    return true;
}

bool ensure_images(uint32_t w, uint32_t h) {
    if (g.w == w && g.h == h && g.out.image) return true;
    if (g.pending) return false;
    const VkCtx& c = vk();
    vkDeviceWaitIdle(c.device);
    destroy_image(g.cam);
    destroy_image(g.game);
    destroy_image(g.out);
    if (g.staging) {
        vkUnmapMemory(c.device, g.staging_memory);
        vkDestroyBuffer(c.device, g.staging, nullptr);
        vkFreeMemory(c.device, g.staging_memory, nullptr);
        g.staging = VK_NULL_HANDLE;
    }
    if (!create_image(g.cam, VK_FORMAT_R16G16B16A16_SFLOAT, w, h) || !create_image(g.game, VK_FORMAT_R16G16B16A16_SFLOAT, w, h) || !create_image(g.out, VK_FORMAT_R16G16_SFLOAT, w, h)) return false;
    g.staging_size = VkDeviceSize(w) * h * 20;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = g.staging_size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(c.device, &bi, nullptr, &g.staging) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c.device, g.staging, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(c.device, &ai, nullptr, &g.staging_memory) != VK_SUCCESS) return false;
    vkBindBufferMemory(c.device, g.staging, g.staging_memory, 0);
    void* p = nullptr;
    vkMapMemory(c.device, g.staging_memory, 0, VK_WHOLE_SIZE, 0, &p);
    g.mapped = static_cast<uint8_t*>(p);
    g.w = w;
    g.h = h;
    return true;
}

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}

float h2f(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return _cvtsh_ss(v);
}

uint8_t to8(float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f)); }

void write_ppm(const std::string& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgb) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
}

std::string dump_dir() {
    const char* d = std::getenv("GOWA_MV_DIR");
    if (!d) d = std::getenv("GOWA_SNAP_DIR");
    return d ? d : ".";
}

}

std::string dump_base(double elapsed) {
    char base[512];
    std::snprintf(base, sizeof base, "%s/mv_%07.1f", dump_dir().c_str(), elapsed);
    return base;
}

double elapsed() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

bool enabled() {
    static const bool e = [] {
        const char* v = std::getenv("GOWA_MV_DEBUG");
        return v && std::atoi(v) != 0;
    }();
    return e || dlssg::active() || dlssnr::requested();
}

VkImage output(VkImageLayout& layout) {
    layout = g.out_layout;
    return g.out.image;
}

const float* camera_consts() { return g.consts; }

void set_camera(const float* fc, size_t vec4_count, uint32_t velocity_offset, uint32_t depth_offset) {
    if (vec4_count < 18) return;
    const int order[8] = {7, 5, 8, 9, 11, 10, 12, 13};
    for (int i = 0; i < 8; i++) std::memcpy(&g.consts[i * 4], fc + order[i] * 4, 16);
    g.have_camera = true;
    g.velocity_offset = velocity_offset;
    g.depth_offset = depth_offset;
}

bool camera(uint32_t& velocity_offset, uint32_t& depth_offset) {
    if (!g.have_camera) return false;
    velocity_offset = g.velocity_offset;
    depth_offset = g.depth_offset;
    return true;
}

bool want_dump(double elapsed) {
    static const double from = env_double("GOWA_MV_DUMP_FROM", 0.0);
    static const uint32_t count = static_cast<uint32_t>(env_double("GOWA_MV_DUMP_COUNT", 4.0));
    static const double every = env_double("GOWA_MV_DUMP_EVERY", 0.0);
    static double next = from;
    if (from <= 0.0 || g.pending || g.dumps >= count || elapsed < next) return false;
    next = elapsed + every;
    return true;
}

int run(VkCommandBuffer cmd, VkImageView velocity, VkImageView depth, uint32_t w, uint32_t h, double elapsed) {
    g.have_camera = false;
    if (!init_pipeline() || !ensure_images(w, h)) return 0;
    static const float yflip = static_cast<float>(env_double("GOWA_MV_YFLIP", 1.0));
    static const float decode = static_cast<float>(env_double("GOWA_MV_GAME_SCALE", 0.1));
    VkPipelineStageFlags cs = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    Image* imgs[3] = {&g.cam, &g.game, &g.out};
    for (Image* i : imgs) barrier(cmd, i->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, cs);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, cs, 0, 1, &mb, 0, nullptr, 0, nullptr);
    VkDescriptorImageInfo ii[5] = {
        {g.sampler, velocity, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {g.sampler, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, g.cam.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, g.game.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, g.out.view, VK_IMAGE_LAYOUT_GENERAL},
    };
    VkWriteDescriptorSet wr[5]{};
    for (uint32_t i = 0; i < 5; i++) {
        wr[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, i, 0, 1, i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii[i], nullptr, nullptr};
    }
    float pc[36];
    std::memcpy(pc, g.consts, 128);
    pc[32] = float(w);
    pc[33] = float(h);
    pc[34] = yflip;
    pc[35] = decode;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.pipeline);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.layout, 0, 5, wr);
    vkCmdPushConstants(cmd, g.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    g.runs++;
    bool dump = want_dump(elapsed);
    VkImageLayout final_layout = dump ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAccessFlags final_access = dump ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_SHADER_READ_BIT;
    VkPipelineStageFlags final_stage = dump ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    for (Image* i : imgs) barrier(cmd, i->image, VK_IMAGE_LAYOUT_GENERAL, final_layout, VK_ACCESS_SHADER_WRITE_BIT, final_access, cs, final_stage);
    g.out_layout = final_layout;
    if (dump) {
        VkDeviceSize off = 0;
        const uint32_t bpp[3] = {8, 8, 4};
        for (uint32_t k = 0; k < 3; k++) {
            VkBufferImageCopy c{};
            c.bufferOffset = off;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageExtent = {w, h, 1};
            vkCmdCopyImageToBuffer(cmd, imgs[k]->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.staging, 1, &c);
            off += VkDeviceSize(w) * h * bpp[k];
        }
        g.pending = true;
        g.pending_time = elapsed;
        g.dumps++;
    }
    return dump ? 2 : 1;
}

void complete() {
    if (!g.pending) return;
    g.pending = false;
    const uint32_t w = g.w, h = g.h;
    const size_t n = size_t(w) * h;
    const uint8_t* cam = g.mapped;
    const uint8_t* game = cam + n * 8;
    const uint8_t* out = game + n * 8;
    static const float yflip = static_cast<float>(env_double("GOWA_MV_YFLIP", 1.0));
    double err_a = 0, err_b = 0, mag = 0, omag = 0, maxo = 0;
    uint64_t cnt = 0, ocnt = 0, clamped = 0;
    std::vector<uint8_t> rc(n * 3), rg(n * 3), ro(n * 3);
    const float k = 4.0f;
    for (size_t i = 0; i < n; i++) {
        float ax = h2f(cam + i * 8), ay = h2f(cam + i * 8 + 2), bx = h2f(cam + i * 8 + 4), by = h2f(cam + i * 8 + 6);
        float gx = h2f(game + i * 8), gy = h2f(game + i * 8 + 2), gm = h2f(game + i * 8 + 4), gd = h2f(game + i * 8 + 6);
        float ox = h2f(out + i * 4), oy = h2f(out + i * 4 + 2);
        bool camera_px = gm > 0.5f;
        float glen = std::sqrt(gx * gx + gy * gy);
        if (camera_px && gd < 0.9999f) {
            if (glen < 0.095f) {
                err_a += std::fabs(ax - gx) + std::fabs(ay - gy);
                err_b += std::fabs(bx - gx) + std::fabs(by - gy);
                mag += glen;
                cnt++;
            } else {
                clamped++;
            }
        }
        float olen = std::sqrt(ox * ox + oy * oy);
        if (!camera_px) {
            omag += olen;
            maxo = std::max<double>(maxo, olen);
            ocnt++;
        }
        float cx = (yflip > 0.5f ? bx : ax) * 0.5f * w, cy = (yflip > 0.5f ? -by : ay) * 0.5f * h;
        float gpx = gx * 0.5f * w, gpy = (yflip > 0.5f ? -gy : gy) * 0.5f * h;
        rc[i * 3] = to8(128 + cx * k);
        rc[i * 3 + 1] = to8(128 + cy * k);
        rc[i * 3 + 2] = 128;
        rg[i * 3] = to8(128 + gpx * k);
        rg[i * 3 + 1] = to8(128 + gpy * k);
        rg[i * 3 + 2] = camera_px ? 128 : 255;
        ro[i * 3] = to8(128 + ox * k);
        ro[i * 3 + 1] = to8(128 + oy * k);
        ro[i * 3 + 2] = camera_px ? 128 : 255;
    }
    std::string bs = dump_base(g.pending_time);
    const char* base = bs.c_str();
    write_ppm(std::string(base) + "_cam.ppm", w, h, rc);
    write_ppm(std::string(base) + "_game.ppm", w, h, rg);
    write_ppm(std::string(base) + "_final.ppm", w, h, ro);
    if (FILE* f = std::fopen((std::string(base) + "_final.rg16f").c_str(), "wb")) {
        uint32_t hdr[2] = {w, h};
        std::fwrite(hdr, 4, 2, f);
        std::fwrite(out, 4, n, f);
        std::fclose(f);
    }
    double px_scale = 0.5 * w;
    LOG_INFO("mv", "dump %s: camera px %llu (clamped %llu) mean |game| %.2f px, mean err vs game: yup %.3f px, ydown %.3f px; object px %llu mean %.2f max %.1f px", base, static_cast<unsigned long long>(cnt),
             static_cast<unsigned long long>(clamped), cnt ? mag / cnt * px_scale : 0.0, cnt ? err_b / cnt * px_scale : 0.0, cnt ? err_a / cnt * px_scale : 0.0, static_cast<unsigned long long>(ocnt), ocnt ? omag / ocnt : 0.0,
             maxo);
}

}
