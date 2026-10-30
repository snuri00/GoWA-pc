#include "runtime/placement.h"
#include "runtime/host.h"
#include "gpu/presenter.h"
#include "gpu/rsx.h"
#include "vk_context.h"
#include "dlssg.h"
#include "dlssnr.h"
#include "osd.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/hooks.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#endif

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
#ifdef _WIN32
    HWND hwnd = nullptr;
#else
    SDL_Window* window = nullptr;
    bool window_failed = false;
#endif
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
    VkCommandBuffer pre = VK_NULL_HANDLE;
    bool pre_recording = false;
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
    bool recording = false;
    VkCtx ctx;
    VkPhysicalDeviceFeatures features{};
};

State g;
std::atomic<bool> g_enabled{false};

#ifdef _WIN32
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
#endif

uint32_t find_memory_impl(uint32_t bits, VkMemoryPropertyFlags flags) {
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
#ifndef _WIN32
    if (caps.currentExtent.width == UINT32_MAX) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(g.window, &w, &h);
        caps.currentExtent.width = std::clamp(static_cast<uint32_t>(w), caps.minImageExtent.width, caps.maxImageExtent.width);
        caps.currentExtent.height = std::clamp(static_cast<uint32_t>(h), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
#endif
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
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (hooks::fps_unlock_target() > 60.5 || dlssg::want_low_latency_present()) {
        uint32_t pm = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(g.ctx.phys, g.surface, &pm, nullptr);
        std::vector<VkPresentModeKHR> modes(pm);
        vkGetPhysicalDeviceSurfacePresentModesKHR(g.ctx.phys, g.surface, &pm, modes.data());
        bool immediate = dlssg::want_low_latency_present() && std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) != modes.end();
        if (immediate) ci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()) ci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) != modes.end()) ci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = g.swapchain;
    VkSwapchainKHR sc;
    if (VkResult r = vkCreateSwapchainKHR(g.device, &ci, nullptr, &sc); r != VK_SUCCESS) {
        LOG_ERROR("gpu", "vkCreateSwapchainKHR %ux%u failed %d", g.swap_extent.width, g.swap_extent.height, int(r));
        return false;
    }
    if (g.swapchain) vkDestroySwapchainKHR(g.device, g.swapchain, nullptr);
    g.swapchain = sc;
    vkGetSwapchainImagesKHR(g.device, g.swapchain, &n, nullptr);
    g.swap_images.resize(n);
    vkGetSwapchainImagesKHR(g.device, g.swapchain, &n, g.swap_images.data());
    dlssg::set_backbuffer(g.swap_extent.width, g.swap_extent.height);
    LOG_INFO("gpu", "swapchain %ux%u, %u images, present mode %d", g.swap_extent.width, g.swap_extent.height, n, int(ci.presentMode));
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
    ai.memoryTypeIndex = find_memory_impl(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(g.device, &ai, nullptr, &g.frame_mem);
    vkBindImageMemory(g.device, g.frame, g.frame_mem, 0);
    g.staging_size = VkDeviceSize(w) * h * 4;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = g.staging_size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkCreateBuffer(g.device, &bi, nullptr, &g.staging);
    vkGetBufferMemoryRequirements(g.device, g.staging, &req);
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory_impl(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(g.device, &ai, nullptr, &g.staging_mem);
    vkBindBufferMemory(g.device, g.staging, g.staging_mem, 0);
    vkMapMemory(g.device, g.staging_mem, 0, g.staging_size, 0, &g.staging_ptr);
    g.frame_w = w;
    g.frame_h = h;
    return true;
}

uint64_t g_frame_serial = 0;

struct PresentSnap {
    double every = 0.0;
    double next = 0.0;
    uint64_t freq = 0, start = 0;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* ptr = nullptr;
    VkDeviceSize size = 0;
    uint32_t w = 0, h = 0;
    bool pending = false;
    char path[512] = {};
};

PresentSnap g_snap;

void snap_write() {
    if (!g_snap.pending) return;
    g_snap.pending = false;
    FILE* f = std::fopen(g_snap.path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", g_snap.w, g_snap.h);
    const uint8_t* src = static_cast<const uint8_t*>(g_snap.ptr);
    bool bgra = g.swap_format == VK_FORMAT_B8G8R8A8_UNORM || g.swap_format == VK_FORMAT_B8G8R8A8_SRGB;
    std::vector<uint8_t> row(size_t(g_snap.w) * 3);
    for (uint32_t y = 0; y < g_snap.h; y++) {
        const uint8_t* p = src + size_t(y) * g_snap.w * 4;
        for (uint32_t x = 0; x < g_snap.w; x++) {
            row[x * 3 + 0] = p[x * 4 + (bgra ? 2 : 0)];
            row[x * 3 + 1] = p[x * 4 + 1];
            row[x * 3 + 2] = p[x * 4 + (bgra ? 0 : 2)];
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    LOG_INFO("gpu", "present snapshot %s", g_snap.path);
}

bool snap_due() {
    if (!g_snap.freq) {
        g_snap.freq = host::qpf();
        g_snap.start = host::qpc();
        const char* e = std::getenv("GOWA_PRESENT_SNAP");
        g_snap.every = e ? std::atof(e) : 0.0;
        g_snap.next = g_snap.every;
    }
    if (g_snap.every <= 0.0) return false;
    uint64_t now;
    now = host::qpc();
    double t = double(now - g_snap.start) / double(g_snap.freq);
    if (t < g_snap.next) return false;
    while (g_snap.next <= t) g_snap.next += g_snap.every;
    const char* d = std::getenv("GOWA_SNAP_DIR");
    std::snprintf(g_snap.path, sizeof g_snap.path, "%s/present_%07.1f.ppm", d ? d : ".", t);
    return true;
}

void snap_record(VkImage img) {
    VkDeviceSize need = VkDeviceSize(g.swap_extent.width) * g.swap_extent.height * 4;
    if (g_snap.size < need) {
        if (g_snap.buf) {
            vkDestroyBuffer(g.device, g_snap.buf, nullptr);
            vkFreeMemory(g.device, g_snap.mem, nullptr);
        }
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = need;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vkCreateBuffer(g.device, &bi, nullptr, &g_snap.buf);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(g.device, g_snap.buf, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = find_memory_impl(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = find_memory_impl(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(g.device, &ai, nullptr, &g_snap.mem);
        vkBindBufferMemory(g.device, g_snap.buf, g_snap.mem, 0);
        vkMapMemory(g.device, g_snap.mem, 0, need, 0, &g_snap.ptr);
        g_snap.size = need;
    }
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {g.swap_extent.width, g.swap_extent.height, 1};
    vkCmdCopyImageToBuffer(g.cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_snap.buf, 1, &c);
    g_snap.w = g.swap_extent.width;
    g_snap.h = g.swap_extent.height;
    g_snap.pending = true;
}

void begin_frame() {
    if (g.recording) return;
    uint64_t a, b, f;
    a = host::qpc();
    if (VkResult wr = vkWaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX); wr != VK_SUCCESS) {
        static int reported = 0;
        if (reported++ < 5) LOG_ERROR("gpu", "vkWaitForFences failed %d", int(wr));
    }
    b = host::qpc();
    snap_write();
    f = host::qpf();
    static double total = 0;
    static uint64_t n = 0;
    total += double(b - a) * 1000.0 / double(f);
    if (++n % 60 == 0) {
        LOG_INFO("gpu", "fence wait %.2f ms per frame", total / 60.0);
        total = 0;
    }
    gpu::rsx::frame_complete();
    g_frame_serial++;
    vkResetCommandBuffer(g.cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g.cmd, &begin);
    g.recording = true;
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
            out[x] = __builtin_bswap32(v) | 0xFF000000u;
        }
    }
}

}

bool enabled() { return g_enabled; }

#ifndef _WIN32
void* native_window() { return g.window; }
#endif

bool init() {
    if (std::getenv("GOWA_HEADLESS")) return false;
    if (!dlssg::load_vulkan() && volkInitialize() != VK_SUCCESS) {
        LOG_WARN("gpu", "Vulkan loader not available, running headless");
        return false;
    }
    std::mutex wm;
    std::condition_variable wcv;
#ifdef _WIN32
    std::thread([&] {
        host::set_thread_name("Window");
        placement::apply("window");
        WNDCLASSW wc{};
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"GOWA";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        LONG ws = dlssnr::mode() == 2 ? 2 : 1;
        RECT r{0, 0, 1280 * ws, 720 * ws};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        HWND h = CreateWindowExW(0, L"GOWA", L"God of War: Ascension", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                                 r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
        {
            std::lock_guard lk(wm);
            g.hwnd = h;
        }
        wcv.notify_all();
        if (std::getenv("GOWA_FOCUS")) SetTimer(h, 1, 2000, nullptr);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_TIMER && msg.hwnd == h && GetForegroundWindow() != h) {
                keybd_event(VK_MENU, 0, 0, 0);
                keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
                SetForegroundWindow(h);
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }).detach();
    {
        std::unique_lock lk(wm);
        wcv.wait(lk, [] { return g.hwnd != nullptr; });
    }
    std::vector<const char*> exts = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
#else
    std::thread([&] {
        host::set_thread_name("Window");
        placement::apply("window");
        SDL_Window* w = nullptr;
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
            int ws = dlssnr::mode() == 2 ? 2 : 1;
            w = SDL_CreateWindow("God of War: Ascension", 1280 * ws, 720 * ws, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        }
        if (!w) LOG_ERROR("gpu", "SDL window creation failed: %s", SDL_GetError());
        {
            std::lock_guard lk(wm);
            g.window = w;
            g.window_failed = !w;
        }
        wcv.notify_all();
        if (!w) return;
        for (;;) {
            SDL_Event e;
            if (!SDL_WaitEventTimeout(&e, 100)) continue;
            switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                LOG_INFO("gpu", "window closed, exiting");
                std::fflush(nullptr);
                std::_Exit(0);
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                g.resized = true;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (SDL_Gamepad* p = SDL_OpenGamepad(e.gdevice.which)) LOG_INFO("input", "gamepad connected: %s", SDL_GetGamepadName(p));
                break;
            default:
                break;
            }
        }
    }).detach();
    {
        std::unique_lock lk(wm);
        wcv.wait(lk, [] { return g.window != nullptr || g.window_failed; });
    }
    if (!g.window) return false;
    std::vector<const char*> exts;
    Uint32 sdl_n = 0;
    if (const char* const* se = SDL_Vulkan_GetInstanceExtensions(&sdl_n)) exts.assign(se, se + sdl_n);
#endif
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "gowa";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    if (VkResult r = vkCreateInstance(&ici, nullptr, &g.instance); r != VK_SUCCESS) {
        LOG_ERROR("gpu", "vkCreateInstance failed %d", int(r));
        return false;
    }
    volkLoadInstance(g.instance);
#ifdef _WIN32
    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd = g.hwnd;
    if (vkCreateWin32SurfaceKHR(g.instance, &sci, nullptr, &g.surface) != VK_SUCCESS) return false;
#else
    if (!SDL_Vulkan_CreateSurface(g.window, g.instance, nullptr, &g.surface)) {
        LOG_ERROR("gpu", "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return false;
    }
#endif
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
    if (!g.phys) {
        LOG_ERROR("gpu", "no Vulkan device with graphics+present support (%u devices)", n);
        return false;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g.phys, &props);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g.queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    std::vector<const char*> dexts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    if (dlssg::active() || dlssnr::requested()) {
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> avail_exts(en);
        vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &en, avail_exts.data());
        for (const char* want : {"VK_NVX_binary_import", "VK_NVX_image_view_handle"}) {
            for (auto& e : avail_exts) {
                if (std::strcmp(e.extensionName, want) == 0) dexts.push_back(want);
            }
        }
    }
    VkPhysicalDeviceFeatures avail{};
    vkGetPhysicalDeviceFeatures(g.phys, &avail);
    VkPhysicalDeviceVulkan13Features a13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 a2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    a2.pNext = &a13;
    vkGetPhysicalDeviceFeatures2(g.phys, &a2);
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.pipelineCreationCacheControl = a13.pipelineCreationCacheControl;
    VkPhysicalDeviceVulkan12Features a12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 q2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    q2.pNext = &a12;
    vkGetPhysicalDeviceFeatures2(g.phys, &q2);
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.bufferDeviceAddress = a12.bufferDeviceAddress;
    f12.timelineSemaphore = a12.timelineSemaphore;
    if (dlssg::active() || dlssnr::requested()) f13.pNext = &f12;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f13;
    f2.features.samplerAnisotropy = avail.samplerAnisotropy;
    f2.features.fillModeNonSolid = avail.fillModeNonSolid;
    f2.features.depthBounds = avail.depthBounds;
    f2.features.depthClamp = avail.depthClamp;
    f2.features.logicOp = avail.logicOp;
    f2.features.independentBlend = avail.independentBlend;
    f2.features.wideLines = avail.wideLines;
    f2.features.largePoints = avail.largePoints;
    f2.features.textureCompressionBC = avail.textureCompressionBC;
    f2.features.shaderClipDistance = avail.shaderClipDistance;
    f2.features.occlusionQueryPrecise = avail.occlusionQueryPrecise;
    f2.features.shaderFloat64 = avail.shaderFloat64;
    f2.features.shaderInt64 = avail.shaderInt64;
    g.features = f2.features;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(dexts.size());
    dci.ppEnabledExtensionNames = dexts.data();
    if (VkResult r = vkCreateDevice(g.phys, &dci, nullptr, &g.device); r != VK_SUCCESS) {
        LOG_ERROR("gpu", "vkCreateDevice failed %d", int(r));
        return false;
    }
    volkLoadDevice(g.device);
    vkGetDeviceQueue(g.device, g.queue_family, 0, &g.queue);
    dlssg::device_ready(g.phys, g.device);
    dlssnr::device_ready(g.instance, g.phys, g.device);
    g.ctx = {g.phys, g.device, g.queue, g.queue_family, g.features, a13.pipelineCreationCacheControl == VK_TRUE};
    if (!create_swapchain()) {
        LOG_ERROR("gpu", "swapchain creation failed");
        return false;
    }
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g.queue_family;
    vkCreateCommandPool(g.device, &pci, nullptr, &g.pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vkAllocateCommandBuffers(g.device, &cai, &g.cmd);
    vkAllocateCommandBuffers(g.device, &cai, &g.pre);
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
    dlssnr::shutdown();
    dlssg::shutdown();
}

void set_display_buffer(uint32_t id, uint32_t offset, uint32_t pitch, uint32_t width, uint32_t height) {
    if (id >= 8) return;
    std::lock_guard lk(g.m);
    g.buffers[id] = {offset, pitch, width, height};
    LOG_INFO("gpu", "display buffer %u: offset 0x%x pitch %u %ux%u", id, offset, pitch, width, height);
}

const VkCtx& vk() { return g.ctx; }

uint64_t frame_serial() { return g_frame_serial; }

VkCommandBuffer frame_cmd() {
    begin_frame();
    return g.cmd;
}

VkCommandBuffer pre_cmd() {
    begin_frame();
    if (!g.pre_recording) {
        vkResetCommandBuffer(g.pre, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(g.pre, &begin);
        g.pre_recording = true;
    }
    return g.pre;
}

uint32_t find_memory(uint32_t bits, VkMemoryPropertyFlags flags) { return find_memory_impl(bits, flags); }

void image_barrier(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
    if (from == to) return;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

namespace {

uint32_t gather_cmds(VkCommandBuffer out[2]) {
    uint32_t n = 0;
    if (g.pre_recording) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(g.pre, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        vkEndCommandBuffer(g.pre);
        g.pre_recording = false;
        out[n++] = g.pre;
    }
    out[n++] = g.cmd;
    return n;
}

void submit_and_present(uint32_t index) {
    dlssg::pre_submit(g.cmd, g.swap_extent.width, g.swap_extent.height);
    vkEndCommandBuffer(g.cmd);
    g.recording = false;
    vkResetFences(g.device, 1, &g.fence);
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkCommandBuffer cmds[2];
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &g.acquired;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = gather_cmds(cmds);
    si.pCommandBuffers = cmds;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g.rendered;
    if (VkResult sr = vkQueueSubmit(g.queue, 1, &si, g.fence); sr != VK_SUCCESS) {
        static int reported = 0;
        if (reported++ < 5) LOG_ERROR("gpu", "vkQueueSubmit failed %d", int(sr));
    }
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g.rendered;
    pi.swapchainCount = 1;
    pi.pSwapchains = &g.swapchain;
    pi.pImageIndices = &index;
    dlssg::post_submit();
    dlssg::present_begin();
    VkResult r = vkQueuePresentKHR(g.queue, &pi);
    dlssg::present_end();
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g.resized = true;
}

void flush_without_present() {
    if (!g.recording) return;
    vkEndCommandBuffer(g.cmd);
    g.recording = false;
    vkResetFences(g.device, 1, &g.fence);
    VkCommandBuffer cmds[2];
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = gather_cmds(cmds);
    si.pCommandBuffers = cmds;
    if (VkResult sr = vkQueueSubmit(g.queue, 1, &si, g.fence); sr != VK_SUCCESS) {
        static int reported = 0;
        if (reported++ < 5) LOG_ERROR("gpu", "vkQueueSubmit failed %d", int(sr));
    }
}

bool acquire(uint32_t& index) {
    if (g.resized) {
        flush_without_present();
        vkDeviceWaitIdle(g.device);
        if (!create_swapchain()) return false;
        g.resized = false;
        begin_frame();
    }
    VkResult r = vkAcquireNextImageKHR(g.device, g.swapchain, UINT64_MAX, g.acquired, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        g.resized = true;
        flush_without_present();
        return false;
    }
    return true;
}

void blit_to_swapchain(VkImage src, uint32_t w, uint32_t h, uint32_t index) {
    VkImage dst = g.swap_images[index];
    barrier(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(w), int32_t(h), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {int32_t(g.swap_extent.width), int32_t(g.swap_extent.height), 1};
    vkCmdBlitImage(g.cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    osd::game_frame();
    osd::draw(g.cmd, dst, g.swap_format, g.swap_extent.width, g.swap_extent.height);
    if (snap_due()) {
        barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        snap_record(dst);
        barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0);
        return;
    }
    barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
}

}

void present_image(VkImage img, VkImageLayout& layout, uint32_t w, uint32_t h) {
    if (!g_enabled) return;
    std::lock_guard lk(g.m);
    begin_frame();
    uint32_t index = 0;
    if (!acquire(index)) return;
    image_barrier(g.cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    blit_to_swapchain(img, w, h, index);
    submit_and_present(index);
}

void flip(uint32_t id) {
    if (!g_enabled || id >= 8) return;
    std::lock_guard lk(g.m);
    const DisplayBuffer d = g.buffers[id];
    if (!d.width || !d.height || !d.pitch) return;
    begin_frame();
    if (!ensure_frame(d.width, d.height)) return;
    uint32_t index = 0;
    if (!acquire(index)) return;
    convert(d);
    barrier(g.frame, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {d.width, d.height, 1};
    vkCmdCopyBufferToImage(g.cmd, g.staging, g.frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(g.frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    blit_to_swapchain(g.frame, d.width, d.height, index);
    submit_and_present(index);
}

bool display_buffer(uint32_t id, uint32_t& offset, uint32_t& pitch, uint32_t& width, uint32_t& height) {
    if (id >= 8) return false;
    std::lock_guard lk(g.m);
    offset = g.buffers[id].offset;
    pitch = g.buffers[id].pitch;
    width = g.buffers[id].width;
    height = g.buffers[id].height;
    return width != 0;
}

}
