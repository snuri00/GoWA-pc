#include "osd.h"

#include "dlssg.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gpu::osd {

namespace {

constexpr uint32_t GW = 5, GH = 7, CW = 6, PAD = 2;
constexpr uint32_t FG_RGB = 0x00E62E, BG_RGB = 0x101010;

struct Glyph {
    char c;
    uint8_t rows[GH];
};

constexpr Glyph FONT[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}}, {'f', {0x06, 0x09, 0x08, 0x1C, 0x08, 0x08, 0x08}},
    {'p', {0x00, 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10}}, {'s', {0x00, 0x00, 0x0E, 0x10, 0x0E, 0x01, 0x1E}},
    {'m', {0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11}}, {'|', {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}}, {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
};

const uint8_t* glyph(char c) {
    for (const Glyph& g : FONT) {
        if (g.c == c) return g.rows;
    }
    return nullptr;
}

struct State {
    bool init = false;
    bool failed = false;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint8_t* ptr = nullptr;
    VkDeviceSize size = 0;
    uint32_t scale = 0;
    uint32_t cols = 0;
    uint32_t bw = 0, bh = 0;
    char text[40] = {};
    char drawn[40] = {};
    bool dirty = true;
    LARGE_INTEGER freq{}, start{};
    uint64_t flips = 0;
    uint64_t presented_start = 0;
};

State s;

void release() {
    const VkCtx& c = vk();
    if (s.buf) vkDestroyBuffer(c.device, s.buf, nullptr);
    if (s.mem) vkFreeMemory(c.device, s.mem, nullptr);
    s.buf = VK_NULL_HANDLE;
    s.mem = VK_NULL_HANDLE;
    s.ptr = nullptr;
    s.size = 0;
}

bool ensure_buffer(VkDeviceSize size) {
    if (s.buf && s.size >= size) return true;
    const VkCtx& c = vk();
    if (s.buf) {
        vkDeviceWaitIdle(c.device);
        release();
    }
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(c.device, &bi, nullptr, &s.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c.device, s.buf, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(c.device, &ai, nullptr, &s.mem) != VK_SUCCESS) {
        release();
        return false;
    }
    vkBindBufferMemory(c.device, s.buf, s.mem, 0);
    void* p = nullptr;
    vkMapMemory(c.device, s.mem, 0, size, 0, &p);
    s.ptr = static_cast<uint8_t*>(p);
    s.size = size;
    s.dirty = true;
    return true;
}

void put(uint8_t* bgra, uint8_t* rgba, uint32_t rgb) {
    uint8_t r = uint8_t(rgb >> 16), g = uint8_t(rgb >> 8), b = uint8_t(rgb);
    bgra[0] = b, bgra[1] = g, bgra[2] = r, bgra[3] = 255;
    rgba[0] = r, rgba[1] = g, rgba[2] = b, rgba[3] = 255;
}

void rasterize() {
    VkDeviceSize plane = VkDeviceSize(s.bw) * s.bh * 4;
    uint8_t* bgra = s.ptr;
    uint8_t* rgba = s.ptr + plane;
    for (uint32_t i = 0; i < s.bw * s.bh; i++) put(bgra + i * 4, rgba + i * 4, BG_RGB);
    uint32_t n = uint32_t(std::strlen(s.text));
    for (uint32_t ci = 0; ci < n && ci < s.cols; ci++) {
        const uint8_t* rows = glyph(s.text[ci]);
        if (!rows) continue;
        for (uint32_t gy = 0; gy < GH; gy++) {
            for (uint32_t gx = 0; gx < GW; gx++) {
                if (!(rows[gy] & (0x10 >> gx))) continue;
                uint32_t x0 = (PAD + ci * CW + gx) * s.scale, y0 = (PAD + gy) * s.scale;
                for (uint32_t y = y0; y < y0 + s.scale; y++) {
                    for (uint32_t x = x0; x < x0 + s.scale; x++) {
                        size_t o = (size_t(y) * s.bw + x) * 4;
                        put(bgra + o, rgba + o, FG_RGB);
                    }
                }
            }
        }
    }
    std::memcpy(s.drawn, s.text, sizeof s.drawn);
    s.dirty = false;
}

void update_text() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!s.start.QuadPart) {
        QueryPerformanceFrequency(&s.freq);
        s.start = now;
        s.flips = 0;
        s.presented_start = dlssg::presented_total();
        std::snprintf(s.text, sizeof s.text, "-- fps");
        return;
    }
    double dt = double(now.QuadPart - s.start.QuadPart) / double(s.freq.QuadPart);
    if (dt < 0.5 || !s.flips) return;
    double fps = double(s.flips) / dt;
    double ms = dt * 1000.0 / double(s.flips);
    uint64_t pres = dlssg::presented_total();
    if (dlssg::generating()) std::snprintf(s.text, sizeof s.text, "%.0f fps %.1f ms | FG %.0f", fps, ms, double(pres - s.presented_start) / dt);
    else std::snprintf(s.text, sizeof s.text, "%.0f fps %.1f ms", fps, ms);
    s.start = now;
    s.flips = 0;
    s.presented_start = pres;
    if (std::strcmp(s.text, s.drawn) != 0) s.dirty = true;
}

}

bool enabled() {
    static const bool on = [] {
        const char* v = std::getenv("GOWA_FPS_OSD");
        return !v || std::atoi(v) != 0;
    }();
    return on;
}

void game_frame() {
    if (enabled()) s.flips++;
}

void draw(VkCommandBuffer cmd, VkImage target, VkFormat format, uint32_t w, uint32_t h) {
    if (!enabled() || s.failed) return;
    bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
    bool rgba = format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB;
    if (!bgra && !rgba) return;
    update_text();
    uint32_t scale = std::max(1u, h / 540);
    uint32_t cols = dlssg::active() ? 25 : 16;
    uint32_t bw = (cols * CW + PAD * 2 - 1) * scale, bh = (GH + PAD * 2) * scale;
    uint32_t mx = 4 * scale, my = 4 * scale;
    if (bw + mx > w || bh + my > h) return;
    if (scale != s.scale || cols != s.cols) {
        s.scale = scale;
        s.cols = cols;
        s.bw = bw;
        s.bh = bh;
        s.dirty = true;
    }
    if (!ensure_buffer(VkDeviceSize(bw) * bh * 8)) {
        s.failed = true;
        return;
    }
    if (s.dirty) rasterize();
    VkBufferImageCopy copy{};
    copy.bufferOffset = rgba ? VkDeviceSize(bw) * bh * 4 : 0;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageOffset = {int32_t(mx), int32_t(my), 0};
    copy.imageExtent = {bw, bh, 1};
    vkCmdCopyBufferToImage(cmd, s.buf, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    VkImage ui = dlssg::ui_begin(cmd, w, h);
    if (ui) {
        copy.bufferOffset = 0;
        vkCmdCopyBufferToImage(cmd, s.buf, ui, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
}

}
