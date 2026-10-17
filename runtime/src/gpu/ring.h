#pragma once

#include "vk_context.h"

#include <cstdint>

namespace gpu {

struct RingAlloc {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    uint8_t* ptr = nullptr;
};

bool ring_alloc(VkDeviceSize size, VkDeviceSize align, RingAlloc& out);
uint64_t frame_serial();

}
