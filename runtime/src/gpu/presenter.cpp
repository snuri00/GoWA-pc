#include "gpu/presenter.h"
#include "runtime/log.h"
#include "runtime/memory.h"

#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <volk.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace gpu {

namespace {

constexpr uint32_t LOCAL_BASE = 0xC0000000;

struct DisplayBuffer {
    uint32_t offset = 0;
    uint32_t pitch = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct State {
    HWND hwnd = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swap_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D swap_extent{};
    std::vector<VkImage> swap_images;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkSemaphore rendered = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    void* staging_ptr = nullptr;
    VkDeviceSize staging_size = 0;
    VkImage frame = VK_NULL_HANDLE;
    VkDeviceMemory frame_mem = VK_NULL_HANDLE;
    uint32_t frame_w = 0;
    uint32_t frame_h = 0;
    DisplayBuffer buffers[8];
    std::mutex m;
    bool ready = false;
    bool resized = false;
};

State g;
std::atomic<bool> g_enabled{false};

LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        LOG_INFO("gpu", "window closed, exiting");
        ExitProcess(0);
    case WM_SIZE:
        g.resized = true;
        return 0;
    default:
        return DefWindowProcW(h, msg, wp, lp);
    }
}

uint32_t find_memory(uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(g.phys, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    return UINT32_MAX;
}

bool create_swapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, g.surface, &caps);
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return false;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surface, &n, formats.data());
    g.swap_format = formats[0].format;
    VkColorSpaceKHR space = formats[0].colorSpace;
    for (auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) {
            g.swap_format = f.format;
            space = f.colorSpace;
        }
    }
    g.swap_extent = caps.currentExtent;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g.surface;
    ci.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount) ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = g.swap_format;
    ci.imageColorSpace = space;
    ci.imageExtent = g.swap_extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = g.swapchain;
    VkSwapchainKHR sc;
    if (vkCreateSwapchainKHR(g.device, &ci, nullptr, &sc) != VK_SUCCESS) return false;
    if (g.swapchain) vkDestroySwapchainKHR(g.device, g.swapchain, nullptr);
    g.swapchain = sc;
    vkGetSwapchainImagesKHR(g.device, g.swapchain, &n, nullptr);
    g.swap_images.resize(n);
    vkGetSwapchainImagesKHR(g.device, g.swapchain, &n, g.swap_images.data());
    return true;
}

bool ensure_frame(uint32_t w, uint32_t h) {
    if (g.frame && g.frame_w == w && g.frame_h == h) return true;
    vkDeviceWaitIdle(g.device);
    if (g.frame) {
        vkDestroyImage(g.device, g.frame, nullptr);
        vkFreeMemory(g.device, g.frame_mem, nullptr);
        vkDestroyBuffer(g.device, g.staging, nullptr);
        vkFreeMemory(g.device, g.staging_mem, nullptr);
    }
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_B8G8R8A8_UNORM;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(g.device, &ii, nullptr, &g.frame) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.device, g.frame, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(g.device, &ai, nullptr, &g.frame_mem);
    vkBindImageMemory(g.device, g.frame, g.frame_mem, 0);
    g.staging_size = VkDeviceSize(w) * h * 4;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = g.staging_size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkCreateBuffer(g.device, &bi, nullptr, &g.staging);
    vkGetBufferMemoryRequirements(g.device, g.staging, &req);
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(g.device, &ai, nullptr, &g.staging_mem);
    vkBindBufferMemory(g.device, g.staging, g.staging_mem, 0);
    vkMapMemory(g.device, g.staging_mem, 0, g.staging_size, 0, &g.staging_ptr);
    g.frame_w = w;
    g.frame_h = h;
    return true;
}

void barrier(VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void convert(const DisplayBuffer& d) {
    auto* dst = static_cast<uint32_t*>(g.staging_ptr);
    for (uint32_t y = 0; y < d.height; y++) {
        const uint8_t* row = vm::ptr(LOCAL_BASE + d.offset + y * d.pitch);
        uint32_t* out = dst + size_t(y) * d.width;
        for (uint32_t x = 0; x < d.width; x++) {
            uint32_t v;
            std::memcpy(&v, row + x * 4, 4);
            out[x] = _byteswap_ulong(v) | 0xFF000000u;
        }
    }
}

}

bool enabled() { return g_enabled; }

bool init() {
    if (std::getenv("GOWA_HEADLESS")) return false;
    if (volkInitialize() != VK_SUCCESS) {
        LOG_WARN("gpu", "Vulkan loader not available, running headless");
        return false;
    }
    std::mutex wm;
    std::condition_variable wcv;
    std::thread([&] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"GOWA";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        RECT r{0, 0, 1280, 720};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        HWND h = CreateWindowExW(0, L"GOWA", L"God of War: Ascension", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                                 r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
        {
            std::lock_guard lk(wm);
            g.hwnd = h;
        }
        wcv.notify_all();
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }).detach();
    {
        std::unique_lock lk(wm);
        wcv.wait(lk, [] { return g.hwnd != nullptr; });
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "gowa";
    app.apiVersion = VK_API_VERSION_1_2;
    const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = exts;
    if (vkCreateInstance(&ici, nullptr, &g.instance) != VK_SUCCESS) return false;
    volkLoadInstance(g.instance);
    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd = g.hwnd;
    if (vkCreateWin32SurfaceKHR(g.instance, &sci, nullptr, &g.surface) != VK_SUCCESS) return false;
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(g.instance, &n, devs.data());
    for (VkPhysicalDevice d : devs) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qs.data());
        for (uint32_t i = 0; i < qn; i++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, g.surface, &present);
            if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                if (!g.phys || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                    g.phys = d;
                    g.queue_family = i;
                }
                break;
            }
        }
    }
    if (!g.phys) return false;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g.phys, &props);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g.queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dexts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dexts;
    if (vkCreateDevice(g.phys, &dci, nullptr, &g.device) != VK_SUCCESS) return false;
    volkLoadDevice(g.device);
    vkGetDeviceQueue(g.device, g.queue_family, 0, &g.queue);
    if (!create_swapchain()) return false;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g.queue_family;
    vkCreateCommandPool(g.device, &pci, nullptr, &g.pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vkAllocateCommandBuffers(g.device, &cai, &g.cmd);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(g.device, &fci, nullptr, &g.fence);
    VkSemaphoreCreateInfo sei{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(g.device, &sei, nullptr, &g.acquired);
    vkCreateSemaphore(g.device, &sei, nullptr, &g.rendered);
    g.ready = true;
    g_enabled = true;
    LOG_INFO("gpu", "Vulkan presenter on %s, swapchain %ux%u", props.deviceName, g.swap_extent.width, g.swap_extent.height);
    return true;
}

void shutdown() {
    if (!g.device) return;
    vkDeviceWaitIdle(g.device);
}

void set_display_buffer(uint32_t id, uint32_t offset, uint32_t pitch, uint32_t width, uint32_t height) {
    if (id >= 8) return;
    std::lock_guard lk(g.m);
    g.buffers[id] = {offset, pitch, width, height};
    LOG_INFO("gpu", "display buffer %u: offset 0x%x pitch %u %ux%u", id, offset, pitch, width, height);
}

void flip(uint32_t id) {
    if (!g_enabled || id >= 8) return;
    std::lock_guard lk(g.m);
    const DisplayBuffer d = g.buffers[id];
    if (!d.width || !d.height || !d.pitch) return;
    if (g.resized) {
        vkDeviceWaitIdle(g.device);
        if (!create_swapchain()) return;
        g.resized = false;
    }
    vkWaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX);
    if (!ensure_frame(d.width, d.height)) return;
    uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(g.device, g.swapchain, UINT64_MAX, g.acquired, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        g.resized = true;
        return;
    }
    vkResetFences(g.device, 1, &g.fence);
    convert(d);
    vkResetCommandBuffer(g.cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g.cmd, &begin);
    barrier(g.frame, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {d.width, d.height, 1};
    vkCmdCopyBufferToImage(g.cmd, g.staging, g.frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(g.frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkImage dst = g.swap_images[index];
    barrier(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(d.width), int32_t(d.height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {int32_t(g.swap_extent.width), int32_t(g.swap_extent.height), 1};
    vkCmdBlitImage(g.cmd, g.frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    vkEndCommandBuffer(g.cmd);
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &g.acquired;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g.rendered;
    vkQueueSubmit(g.queue, 1, &si, g.fence);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g.rendered;
    pi.swapchainCount = 1;
    pi.pSwapchains = &g.swapchain;
    pi.pImageIndices = &index;
    r = vkQueuePresentKHR(g.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g.resized = true;
}

}
