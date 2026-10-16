#include "ring.h"
#include "runtime/log.h"

namespace gpu {

namespace {

constexpr VkDeviceSize RING_SIZE = 256ull << 20;

VkBuffer g_buffer = VK_NULL_HANDLE;
VkDeviceMemory g_memory = VK_NULL_HANDLE;
uint8_t* g_ptr = nullptr;
VkDeviceSize g_head = 0;
uint64_t g_serial = ~0ull;

bool create() {
    const VkCtx& c = vk();
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = RING_SIZE;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(c.device, &bi, nullptr, &g_buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c.device, g_buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(c.device, &ai, nullptr, &g_memory) != VK_SUCCESS) return false;
    vkBindBufferMemory(c.device, g_buffer, g_memory, 0);
    vkMapMemory(c.device, g_memory, 0, RING_SIZE, 0, reinterpret_cast<void**>(&g_ptr));
    return true;
}

}

bool ring_alloc(VkDeviceSize size, VkDeviceSize align, RingAlloc& out) {
    if (!g_buffer && !create()) return false;
    uint64_t serial = frame_serial();
    if (serial != g_serial) {
        g_serial = serial;
        g_head = 0;
    }
    VkDeviceSize start = (g_head + align - 1) & ~(align - 1);
    if (start + size > RING_SIZE) {
        LOG_WARN("gpu", "ring buffer exhausted in one frame (%llu bytes requested)", static_cast<unsigned long long>(size));
        return false;
    }
    g_head = start + size;
    out.buffer = g_buffer;
    out.offset = start;
    out.ptr = g_ptr + start;
    return true;
}

}
