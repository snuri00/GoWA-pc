#pragma once

#include "vk_context.h"

#include <cstdint>
#include <vector>

namespace gpu {

struct TextureRegs {
    uint32_t offset = 0;
    uint32_t format = 0;
    uint32_t address = 0;
    uint32_t control0 = 0;
    uint32_t control1 = 0;
    uint32_t filter = 0;
    uint32_t rect = 0;
    uint32_t control3 = 0;
    uint32_t border = 0;
};

struct TextureRegion {
    uint64_t offset = 0;
    uint32_t level = 0;
    uint32_t layer = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;
};

struct TextureLayout {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkComponentMapping mapping{};
    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
    uint32_t base = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;
    uint32_t levels = 1;
    uint32_t layers = 1;
    uint32_t source_size = 0;
    uint64_t decoded_size = 0;
    bool is_depth = false;
    std::vector<TextureRegion> regions;
};

bool describe_texture(const TextureRegs& regs, TextureLayout& out);
void decode_texture(const uint8_t* src, const TextureRegs& regs, const TextureLayout& layout, uint8_t* dst);
VkComponentMapping texture_mapping(uint32_t base_format, uint32_t control1);
VkComponentMapping remap_mapping(const VkComponentSwizzle base[4], bool remap, uint32_t control1);

}
