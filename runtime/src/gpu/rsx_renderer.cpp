#include "gpu/presenter.h"
#include "gpu/rsx.h"
#include "runtime/log.h"
#include "vk_context.h"
#include "shader_compiler.h"
#include "vp_decompiler.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>

#include <map>
#include <memory>
#include <tuple>
#include <vector>

namespace gpu::rsx {

namespace {

enum : uint32_t {
    SURFACE_CLIP_HORIZONTAL = 0x0200,
    SURFACE_CLIP_VERTICAL = 0x0204,
    SURFACE_FORMAT = 0x0208,
    SURFACE_PITCH_A = 0x020C,
    SURFACE_COLOR_AOFFSET = 0x0210,
    SURFACE_ZETA_OFFSET = 0x0214,
    SURFACE_COLOR_BOFFSET = 0x0218,
    SURFACE_PITCH_B = 0x021C,
    SURFACE_COLOR_TARGET = 0x0220,
    SURFACE_PITCH_Z = 0x022C,
    SURFACE_PITCH_C = 0x0280,
    SURFACE_PITCH_D = 0x0284,
    SURFACE_COLOR_COFFSET = 0x0288,
    SURFACE_COLOR_DOFFSET = 0x028C,
    CONTEXT_DMA_COLOR_A = 0x0194,
    CONTEXT_DMA_COLOR_B = 0x018C,
    CONTEXT_DMA_ZETA = 0x0198,
    ZSTENCIL_CLEAR_VALUE = 0x1D8C,
    COLOR_CLEAR_VALUE = 0x1D90,
    CLEAR_SURFACE = 0x1D94,
    SCISSOR_HORIZONTAL = 0x08C0,
    SCISSOR_VERTICAL = 0x08C4,
    TRANSFORM_PROGRAM = 0x0B80,
    TRANSFORM_PROGRAM_LOAD = 0x1E9C,
    TRANSFORM_PROGRAM_START = 0x1EA0,
    TRANSFORM_CONSTANT_LOAD = 0x1EFC,
    TRANSFORM_CONSTANT = 0x1F00,
    BEGIN_END = 0x1808,
};

constexpr uint32_t CELL_GCM_LOCATION_LOCAL = 0xFEED0000;

struct Surface {
    uint32_t offset = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

uint32_t g_regs[0x4000];
std::map<std::tuple<uint32_t, VkFormat, uint32_t, uint32_t>, std::unique_ptr<Surface>> g_surfaces;
uint64_t g_clears = 0;
uint32_t g_vp_ucode[544 * 4];
uint32_t g_vp_load_word = 0;
float g_vp_constants[468 * 4];
struct VpEntry {
    bool ok = false;
};
std::unordered_map<uint64_t, VpEntry> g_vp_cache;
uint32_t g_vp_failures = 0;

uint32_t R(uint32_t method) { return g_regs[(method >> 2) & 0x3FFF]; }

uint64_t hash_program(uint32_t start) {
    uint64_t h = 1469598103934665603ull ^ start;
    for (uint32_t s = start; s < 544; s++) {
        for (uint32_t k = 0; k < 4; k++) {
            h ^= g_vp_ucode[s * 4 + k];
            h *= 1099511628211ull;
        }
        if (g_vp_ucode[s * 4 + 3] & 1) {
            bool flow = false;
            for (uint32_t t = start; t <= s; t++) {
                uint32_t sca = (g_vp_ucode[t * 4 + 1] >> 27) & 0x1F;
                if (sca >= 0x08 && sca <= 0x12 && sca != 0x0D && sca != 0x0E && sca != 0x0F && sca != 0x10) flow = true;
            }
            if (!flow) break;
        }
    }
    return h;
}

void check_vertex_program() {
    uint32_t start = R(TRANSFORM_PROGRAM_START);
    uint64_t h = hash_program(start);
    if (g_vp_cache.count(h)) return;
    VpEntry& e = g_vp_cache[h];
    VertexProgramInfo vp = decompile_vertex_program(g_vp_ucode, start);
    std::vector<uint32_t> spirv;
    std::string log;
    e.ok = vp.valid && compile_glsl(ShaderStage::Vertex, vp.glsl, spirv, log);
    if (!e.ok && g_vp_failures++ < 20) {
        char fn[64];
        std::snprintf(fn, sizeof fn, "vp_fail_%016llx.vert", static_cast<unsigned long long>(h));
        if (FILE* f = std::fopen(fn, "w")) {
            std::fputs(vp.glsl.c_str(), f);
            std::fputs("\n/*\n", f);
            std::fputs(log.c_str(), f);
            std::fputs("\n*/\n", f);
            std::fclose(f);
        }
        LOG_WARN("rsx", "vertex program %016llx failed to compile (start %u)", static_cast<unsigned long long>(h), start);
    }
    LOG_INFO("rsx", "vertex program %016llx start %u: %s (%zu seen, %u failures)", static_cast<unsigned long long>(h), start, e.ok ? "ok" : "FAILED", g_vp_cache.size(), g_vp_failures);
}


VkFormat color_format(uint32_t fmt) {
    switch (fmt & 0x1F) {
    case 0x3: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case 0x5: case 0x8: return VK_FORMAT_B8G8R8A8_UNORM;
    case 0x9: return VK_FORMAT_R8_UNORM;
    case 0xB: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0xC: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0xD: return VK_FORMAT_R32_SFLOAT;
    case 0xF: return VK_FORMAT_R8G8B8A8_UNORM;
    default: return VK_FORMAT_B8G8R8A8_UNORM;
    }
}

VkFormat depth_format(uint32_t fmt) { return ((fmt >> 5) & 7) == 1 ? VK_FORMAT_D16_UNORM : VK_FORMAT_D24_UNORM_S8_UINT; }

Surface* surface(uint32_t offset, VkFormat format, uint32_t w, uint32_t h, bool depth) {
    auto key = std::make_tuple(offset, format, w, h);
    auto it = g_surfaces.find(key);
    if (it != g_surfaces.end()) return it->second.get();
    const VkCtx& c = vk();
    auto s = std::make_unique<Surface>();
    s->offset = offset;
    s->width = w;
    s->height = h;
    s->format = format;
    s->aspect = depth ? (format == VK_FORMAT_D16_UNORM ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (vkCreateImage(c.device, &ii, nullptr, &s->image) != VK_SUCCESS) return nullptr;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(c.device, s->image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(c.device, &ai, nullptr, &s->memory);
    vkBindImageMemory(c.device, s->image, s->memory, 0);
    LOG_INFO("rsx", "new %s surface at local 0x%x %ux%u format %d", depth ? "depth" : "color", offset, w, h, int(format));
    Surface* raw = s.get();
    g_surfaces[key] = std::move(s);
    return raw;
}

uint32_t clip_w() { return R(SURFACE_CLIP_HORIZONTAL) >> 16; }
uint32_t clip_h() { return R(SURFACE_CLIP_VERTICAL) >> 16; }

std::vector<Surface*> color_targets() {
    std::vector<Surface*> out;
    uint32_t w = clip_w(), h = clip_h();
    if (!w || !h) return out;
    VkFormat f = color_format(R(SURFACE_FORMAT));
    uint32_t target = R(SURFACE_COLOR_TARGET);
    struct Slot { uint32_t bit; uint32_t offset_reg; uint32_t dma_reg; };
    const Slot slots[4] = {{1, SURFACE_COLOR_AOFFSET, CONTEXT_DMA_COLOR_A}, {2, SURFACE_COLOR_BOFFSET, CONTEXT_DMA_COLOR_B}, {4, SURFACE_COLOR_COFFSET, 0}, {8, SURFACE_COLOR_DOFFSET, 0}};
    uint32_t mask = 0;
    switch (target) {
    case 0x01: mask = 1; break;
    case 0x02: mask = 2; break;
    case 0x13: mask = 3; break;
    case 0x17: mask = 7; break;
    case 0x1F: mask = 15; break;
    default: mask = 0; break;
    }
    for (const Slot& s : slots) {
        if (!(mask & s.bit)) continue;
        if (s.dma_reg && R(s.dma_reg) != CELL_GCM_LOCATION_LOCAL) continue;
        if (Surface* surf = surface(R(s.offset_reg), f, w, h, false)) out.push_back(surf);
    }
    return out;
}

void clear(uint32_t mask) {
    VkCommandBuffer cmd = frame_cmd();
    if (mask & 0xF0) {
        uint32_t v = R(COLOR_CLEAR_VALUE);
        VkClearColorValue cv{};
        cv.float32[0] = ((v >> 16) & 0xFF) / 255.0f;
        cv.float32[1] = ((v >> 8) & 0xFF) / 255.0f;
        cv.float32[2] = (v & 0xFF) / 255.0f;
        cv.float32[3] = ((v >> 24) & 0xFF) / 255.0f;
        for (Surface* s : color_targets()) {
            image_barrier(cmd, s->image, s->aspect, s->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            s->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, s->image, s->layout, &cv, 1, &range);
        }
    }
    if (mask & 0x3) {
        uint32_t w = clip_w(), h = clip_h();
        if (w && h && R(CONTEXT_DMA_ZETA) == CELL_GCM_LOCATION_LOCAL) {
            VkFormat df = depth_format(R(SURFACE_FORMAT));
            if (Surface* s = surface(R(SURFACE_ZETA_OFFSET), df, w, h, true)) {
                uint32_t v = R(ZSTENCIL_CLEAR_VALUE);
                VkClearDepthStencilValue dv{};
                if (df == VK_FORMAT_D16_UNORM) {
                    dv.depth = (v & 0xFFFF) / 65535.0f;
                } else {
                    dv.depth = (v >> 8) / 16777215.0f;
                    dv.stencil = v & 0xFF;
                }
                image_barrier(cmd, s->image, s->aspect, s->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                s->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                VkImageSubresourceRange range{s->aspect, 0, 1, 0, 1};
                vkCmdClearDepthStencilImage(cmd, s->image, s->layout, &dv, 1, &range);
            }
        }
    }
    g_clears++;
}

}

uint32_t reg(uint32_t method) { return R(method); }

void method(uint32_t reg, uint32_t arg) {
    if (reg >= TRANSFORM_PROGRAM && reg < TRANSFORM_PROGRAM + 128) {
        uint32_t load = R(TRANSFORM_PROGRAM_LOAD);
        if (g_vp_load_word / 4 != load && (g_vp_load_word % 4) == 0) g_vp_load_word = load * 4;
        if (g_vp_load_word < 544 * 4) g_vp_ucode[g_vp_load_word] = arg;
        g_vp_load_word++;
        g_regs[(TRANSFORM_PROGRAM_LOAD >> 2)] = g_vp_load_word / 4;
        return;
    }
    if (reg == TRANSFORM_PROGRAM_LOAD) g_vp_load_word = arg * 4;
    if (reg >= TRANSFORM_CONSTANT && reg < TRANSFORM_CONSTANT + 128) {
        uint32_t idx = R(TRANSFORM_CONSTANT_LOAD) * 4 + ((reg - TRANSFORM_CONSTANT) >> 2);
        if (idx < 468 * 4) std::memcpy(&g_vp_constants[idx], &arg, 4);
        return;
    }
    g_regs[(reg >> 2) & 0x3FFF] = arg;
    if (!enabled()) return;
    switch (reg) {
    case CLEAR_SURFACE: clear(arg); break;
    case BEGIN_END:
        if (arg) check_vertex_program();
        break;
    default: break;
    }
}

bool present(uint32_t buffer_id) {
    if (!enabled()) return false;
    uint32_t offset, pitch, width, height;
    if (!display_buffer(buffer_id, offset, pitch, width, height)) return false;
    for (auto& [key, s] : g_surfaces) {
        if (s->offset == offset && s->aspect == VK_IMAGE_ASPECT_COLOR_BIT && s->width >= width && s->height >= height) {
            present_image(s->image, s->layout, width, height);
            return true;
        }
    }
    return false;
}

}
