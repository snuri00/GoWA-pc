#pragma once

#include "vk_context.h"

#include <cstdint>

namespace gpu::dlssnr {

int mode();
bool requested();
bool active();
void device_ready(VkInstance instance, VkPhysicalDevice phys, VkDevice device);
void capture_inputs(VkCommandBuffer cmd, VkImage mv, VkImageLayout mv_layout, VkImage depth, VkImageLayout depth_layout, uint32_t w, uint32_t h);
bool pending();
bool evaluate(VkCommandBuffer cmd, VkImage src, VkImageLayout layout, uint32_t w, uint32_t h);
bool output(VkImage& image, VkImageLayout& layout, uint32_t& w, uint32_t& h);
bool compose(VkCommandBuffer cmd, VkImageView final_view, VkImageLayout final_layout, uint32_t w, uint32_t h, VkImage& image, VkImageLayout*& layout, uint32_t& ow, uint32_t& oh);
bool snapshot_images(VkCommandBuffer cmd, VkImage& in, uint32_t& w, uint32_t& h, VkImage& out, uint32_t& ow, uint32_t& oh);
void end_frame();
void shutdown();

}
