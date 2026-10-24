#pragma once

#include "vk_context.h"

#include <cstdint>

namespace gpu::osd {

bool enabled();
void game_frame();
void draw(VkCommandBuffer cmd, VkImage target, VkFormat format, uint32_t w, uint32_t h);

}
