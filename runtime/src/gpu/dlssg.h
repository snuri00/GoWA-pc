#pragma once

#include "vk_context.h"

#include <cstdint>

namespace gpu::dlssg {

bool requested();
bool load_vulkan();
bool active();
void device_ready(VkPhysicalDevice phys, VkDevice device);
bool want_low_latency_present();
void set_backbuffer(uint32_t w, uint32_t h);
void capture_inputs(VkCommandBuffer cmd, VkImage mv, VkImageLayout mv_layout, VkImage depth, VkImageLayout depth_layout, uint32_t w, uint32_t h, const float* consts);
bool hudless_pending();
void capture_hudless(VkCommandBuffer cmd, VkImage src, VkImageLayout layout, uint32_t w, uint32_t h);
void pre_submit(VkCommandBuffer cmd, uint32_t back_w, uint32_t back_h);
void post_submit();
void present_begin();
void present_end();
void shutdown();

}
