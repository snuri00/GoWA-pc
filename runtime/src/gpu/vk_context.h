#pragma once

#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <volk.h>

#include <cstdint>

namespace gpu {

struct VkCtx {
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
};

const VkCtx& vk();
VkCommandBuffer frame_cmd();
uint32_t find_memory(uint32_t bits, VkMemoryPropertyFlags flags);
void image_barrier(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to);
void present_image(VkImage img, VkImageLayout& layout, uint32_t w, uint32_t h);

}
