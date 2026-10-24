#include "dlssg.h"

#include "runtime/log.h"
#include "runtime/memory.h"

#include <cstdlib>
#include <cstring>

#ifdef GOWA_HAVE_STREAMLINE

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_matrix_helpers.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>

namespace gpu::dlssg {

namespace {

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage = 0;
    uint32_t w = 0, h = 0;
};

struct Slot {
    Image mv, depth, hudless, ui;
    bool ui_ready = false;
};

struct State {
    bool tried = false;
    bool ok = false;
    HMODULE interposer = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    Slot slots[2];
    uint32_t slot = 0;
    uint32_t back_w = 0, back_h = 0;
    sl::FrameToken* token = nullptr;
    uint32_t frame = 0;
    bool inputs = false;
    bool hudless = false;
    bool ui = false;
    uint64_t presented_total = 0;
    bool prev_valid = false;
    bool fg_on = false;
    bool options_set = false;
    float consts[32] = {};
    uint64_t base_frames = 0;
    uint64_t presented = 0;
    uint64_t fg_frames = 0;
    uint64_t hud_frames = 0;
    std::chrono::steady_clock::time_point stat_start;
    std::wstring sl_dir, ngx_dir, log_dir;
};

State g;

std::wstring widen(const std::string& s) { return std::filesystem::path(s).wstring(); }

std::filesystem::path exe_dir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

std::filesystem::path find_ngx_dir() {
    if (const char* d = std::getenv("GOWA_DLSS_DIR")) return std::filesystem::path(d);
    std::error_code ec;
    for (std::filesystem::path p = exe_dir(); !p.empty(); p = p.parent_path()) {
        if (std::filesystem::exists(p / "dlss" / "nvngx_dlssg.dll", ec)) return p / "dlss";
        if (p == p.parent_path()) break;
    }
#ifdef GOWA_DLSS_DEFAULT_DIR
    return std::filesystem::path(GOWA_DLSS_DEFAULT_DIR);
#else
    return exe_dir() / "dlss";
#endif
}

void sl_log(sl::LogType type, const char* msg) {
    if (type == sl::LogType::eError) LOG_ERROR("sl", "%s", msg);
    else if (type == sl::LogType::eWarn) LOG_WARN("sl", "%s", msg);
}

bool create_image(Image& img, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    if (vkCreateImage(g.device, &ii, nullptr, &img.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.device, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g.device, &ai, nullptr, &img.memory) != VK_SUCCESS) return false;
    vkBindImageMemory(g.device, img.image, img.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    if (vkCreateImageView(g.device, &vi, nullptr, &img.view) != VK_SUCCESS) return false;
    img.format = format;
    img.usage = usage;
    img.w = w;
    img.h = h;
    return true;
}

void destroy_image(Image& img) {
    if (img.view) vkDestroyImageView(g.device, img.view, nullptr);
    if (img.image) vkDestroyImage(g.device, img.image, nullptr);
    if (img.memory) vkFreeMemory(g.device, img.memory, nullptr);
    img = {};
}

bool ensure(Image& img, VkFormat format, uint32_t w, uint32_t h, VkImageAspectFlags aspect) {
    if (img.image && img.w == w && img.h == h && img.format == format) return true;
    if (img.image) {
        vkDeviceWaitIdle(g.device);
        destroy_image(img);
    }
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (aspect == VK_IMAGE_ASPECT_COLOR_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (!create_image(img, format, w, h, usage, aspect)) {
        LOG_ERROR("dlssg", "cannot create %ux%u input image (format %d)", w, h, int(format));
        destroy_image(img);
        return false;
    }
    return true;
}

void barrier(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void copy_into(VkCommandBuffer cmd, VkImage src, VkImageLayout layout, VkImageAspectFlags aspect, Image& dst) {
    if (layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) barrier(cmd, src, aspect, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(cmd, dst.image, aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy c{};
    c.srcSubresource = {aspect, 0, 0, 1};
    c.dstSubresource = {aspect, 0, 0, 1};
    c.extent = {dst.w, dst.h, 1};
    vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(cmd, dst.image, aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) barrier(cmd, src, aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout);
}

sl::float4x4 rows(const float* r) {
    sl::float4x4 m;
    for (uint32_t i = 0; i < 4; i++) m[i] = sl::float4(r[i * 4], r[i * 4 + 1], r[i * 4 + 2], r[i * 4 + 3]);
    return m;
}

sl::float4x4 identity() {
    const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    return rows(id);
}

float read_be_float(uint32_t ea) {
    uint32_t v = vm::read32(ea);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

sl::float4x4 guest_matrix(uint32_t ea) {
    float r[16];
    for (uint32_t i = 0; i < 16; i++) r[i] = read_be_float(ea + i * 4);
    return rows(r);
}

bool finite_matrix(const sl::float4x4& m) {
    for (uint32_t i = 0; i < 4; i++) {
        const sl::float4& r = m[i];
        if (!std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.z) || !std::isfinite(r.w)) return false;
    }
    return true;
}

sl::float3 norm3(float x, float y, float z) {
    float l = std::sqrt(x * x + y * y + z * z);
    if (l < 1e-12f) return sl::float3(0, 0, 0);
    return sl::float3(x / l, y / l, z / l);
}

void fill_constants(sl::Constants& c, bool valid) {
    const float s_rows[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 2, 0, 0, 0, -1, 1};
    const float t_rows[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0.5f, 1};
    sl::float4x4 s = rows(s_rows), t = rows(t_rows);
    c.clipToPrevClip = identity();
    c.prevClipToClip = identity();
    if (valid) {
        sl::float4x4 inv = rows(g.consts), prev = rows(g.consts + 16), a, b, m;
        sl::matrixMul(a, s, inv);
        sl::matrixMul(b, a, prev);
        sl::matrixMul(m, b, t);
        if (finite_matrix(m)) {
            c.clipToPrevClip = m;
            sl::matrixFullInvert(c.prevClipToClip, m);
        }
    }
    sl::float4x4 proj = guest_matrix(0x00994640);
    sl::float4x4 view_to_clip;
    sl::matrixMul(view_to_clip, proj, t);
    if (!finite_matrix(view_to_clip) || std::fabs(view_to_clip[0].x) < 1e-6f) {
        const float d[16] = {1.96017f, 0, 0, 0, 0, 3.48474f, 0, 0, 0, 0, -1.00001f, -1, 0, 0, -0.5f, 0};
        proj = rows(d);
        sl::matrixMul(view_to_clip, proj, t);
    }
    c.cameraViewToClip = view_to_clip;
    sl::matrixFullInvert(c.clipToCameraView, view_to_clip);
    c.clipToLensClip = identity();
    c.jitterOffset = sl::float2(0, 0);
    c.mvecScale = sl::float2(g.slots[g.slot].mv.w ? 1.0f / g.slots[g.slot].mv.w : 1.0f, g.slots[g.slot].mv.h ? 1.0f / g.slots[g.slot].mv.h : 1.0f);
    c.cameraPinholeOffset = sl::float2(0, 0);
    sl::float4x4 iv = guest_matrix(0x009946c0);
    c.cameraRight = norm3(iv[0].x, iv[0].y, iv[0].z);
    c.cameraUp = norm3(iv[1].x, iv[1].y, iv[1].z);
    c.cameraFwd = norm3(-iv[2].x, -iv[2].y, -iv[2].z);
    c.cameraPos = sl::float3(iv[3].x, iv[3].y, iv[3].z);
    if (!std::isfinite(c.cameraPos.x) || !std::isfinite(c.cameraRight.x) || !std::isfinite(c.cameraUp.x) || !std::isfinite(c.cameraFwd.x)) {
        c.cameraRight = sl::float3(1, 0, 0);
        c.cameraUp = sl::float3(0, 1, 0);
        c.cameraFwd = sl::float3(0, 0, -1);
        c.cameraPos = sl::float3(0, 0, 0);
    }
    float p00 = proj[0].x, p11 = proj[1].y, p22 = proj[2].z, p32 = proj[3].z;
    c.cameraNear = 0.25f;
    float far_plane = 10000.0f;
    if (std::isfinite(p22) && std::isfinite(p32) && std::fabs(p22 + 1.0f) > 1e-7f) {
        float n = p32 / (p22 - 1.0f);
        float f = p32 / (p22 + 1.0f);
        if (n > 0.0f && std::isfinite(n)) c.cameraNear = n;
        if (f > c.cameraNear && std::isfinite(f)) far_plane = f;
    }
    c.cameraFar = far_plane;
    c.cameraFOV = (std::isfinite(p11) && p11 > 0.01f) ? 2.0f * std::atan(1.0f / p11) : 0.56f;
    c.cameraAspectRatio = (std::isfinite(p00) && p00 > 0.01f) ? p11 / p00 : 16.0f / 9.0f;
    c.motionVectorsInvalidValue = sl::INVALID_FLOAT;
    c.depthInverted = sl::Boolean::eFalse;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.orthographicProjection = sl::Boolean::eFalse;
    c.motionVectorsDilated = sl::Boolean::eFalse;
    c.motionVectorsJittered = sl::Boolean::eFalse;
    c.reset = (!valid || !g.prev_valid) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
}

sl::Resource resource(Image& img) {
    sl::Resource r(sl::ResourceType::eTex2d, img.image, img.memory, img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    r.width = img.w;
    r.height = img.h;
    r.nativeFormat = img.format;
    r.mipLevels = 1;
    r.arrayLayers = 1;
    r.flags = 0;
    r.usage = img.usage;
    return r;
}

void next_token() {
    uint32_t idx = g.frame;
    if (SL_FAILED(res, slGetNewFrameToken(g.token, &idx))) {
        g.token = nullptr;
        return;
    }
    slReflexSleep(*g.token);
    slPCLSetMarker(sl::PCLMarker::eSimulationStart, *g.token);
}

void set_options(bool on) {
    if (g.options_set && on == g.fg_on) return;
    sl::DLSSGOptions o{};
    o.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    o.numFramesToGenerate = 1;
    o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    if (std::getenv("GOWA_DLSSG_ONLY_GENERATED")) o.flags |= sl::DLSSGFlags::eShowOnlyInterpolatedFrame;
    if (SL_FAILED(res, slDLSSGSetOptions(sl::ViewportHandle(0), o))) {
        LOG_WARN("dlssg", "slDLSSGSetOptions failed %d", int(res));
        return;
    }
    if (g.options_set || on) LOG_INFO("dlssg", "frame generation %s (frame %u)", on ? "on" : "off", g.frame);
    g.fg_on = on;
    g.options_set = true;
}

}

bool requested() {
    static const bool r = [] {
        const char* v = std::getenv("GOWA_DLSSG");
        return v && std::atoi(v) != 0;
    }();
    return r;
}

bool active() { return g.ok; }

bool load_vulkan() {
    if (g.tried) return g.ok;
    g.tried = true;
    if (!requested()) return false;
    std::filesystem::path dir = exe_dir();
    if (const char* d = std::getenv("GOWA_SL_DIR")) dir = d;
    std::filesystem::path ngx = find_ngx_dir();
    std::error_code ec;
    if (!std::filesystem::exists(ngx / "nvngx_dlssg.dll", ec)) {
        LOG_ERROR("dlssg", "nvngx_dlssg.dll not found in %s (set GOWA_DLSS_DIR)", ngx.string().c_str());
        return false;
    }
    std::filesystem::path ip = dir / "sl.interposer.dll";
    g.interposer = LoadLibraryW(ip.wstring().c_str());
    if (!g.interposer) {
        LOG_ERROR("dlssg", "cannot load %s (error %lu)", ip.string().c_str(), GetLastError());
        return false;
    }
    g.sl_dir = dir.wstring();
    g.ngx_dir = ngx.wstring();
    const char* ld = std::getenv("GOWA_SNAP_DIR");
    g.log_dir = ld ? widen(ld) : std::filesystem::current_path().wstring();
    const wchar_t* paths[] = {g.sl_dir.c_str(), g.ngx_dir.c_str()};
    sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = std::getenv("GOWA_SL_VERBOSE") ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    pref.pathsToPlugins = paths;
    pref.numPathsToPlugins = 2;
    pref.pathToLogsAndData = g.log_dir.c_str();
    pref.logMessageCallback = sl_log;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 3;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "1.0.0";
    pref.projectId = "6c4b1d2e-7f3a-4e59-9a1b-0d2c3e4f5a6b";
    pref.renderAPI = sl::RenderAPI::eVulkan;
    if (SL_FAILED(res, slInit(pref, sl::kSDKVersion))) {
        LOG_ERROR("dlssg", "slInit failed %d", int(res));
        return false;
    }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(g.interposer, "vkGetInstanceProcAddr"));
    if (!gipa) {
        LOG_ERROR("dlssg", "sl.interposer.dll has no vkGetInstanceProcAddr");
        slShutdown();
        return false;
    }
    volkInitializeCustom(gipa);
    g.ok = true;
    LOG_INFO("dlssg", "Streamline initialised (plugins %s, NGX %s, log dir %s)", dir.string().c_str(), ngx.string().c_str(), std::filesystem::path(g.log_dir).string().c_str());
    return true;
}

void device_ready(VkPhysicalDevice phys, VkDevice device) {
    if (!g.ok) return;
    g.phys = phys;
    g.device = device;
    sl::AdapterInfo ai{};
    ai.vkPhysicalDevice = phys;
    sl::Result sup = slIsFeatureSupported(sl::kFeatureDLSS_G, ai);
    sl::FeatureRequirements req{};
    slGetFeatureRequirements(sl::kFeatureDLSS_G, req);
    sl::FeatureVersion ver{};
    slGetFeatureVersion(sl::kFeatureDLSS_G, ver);
    LOG_INFO("dlssg", "DLSS-G supported: %s (result %d), vulkan flag %d, SL %u.%u.%u, NGX %u.%u.%u", sup == sl::Result::eOk ? "yes" : "no", int(sup),
             int((req.flags & sl::FeatureRequirementFlags::eVulkanSupported) != 0), ver.versionSL.major, ver.versionSL.minor, ver.versionSL.build, ver.versionNGX.major, ver.versionNGX.minor,
             ver.versionNGX.build);
    if (sup != sl::Result::eOk) {
        g.ok = false;
        return;
    }
    sl::ReflexOptions ro{};
    ro.mode = sl::ReflexMode::eLowLatency;
    double limit = 0.0;
    if (const char* l = std::getenv("GOWA_DLSSG_LIMIT")) limit = std::atof(l);
    ro.frameLimitUs = limit > 1.0 ? static_cast<uint32_t>(1e6 / limit) : 0;
    if (SL_FAILED(res, slReflexSetOptions(ro))) LOG_WARN("dlssg", "slReflexSetOptions failed %d", int(res));
    else LOG_INFO("dlssg", "Reflex low latency, frame limit %u us", ro.frameLimitUs);
    g.stat_start = std::chrono::steady_clock::now();
    next_token();
}

bool want_low_latency_present() { return g.ok; }

void set_backbuffer(uint32_t w, uint32_t h) {
    g.back_w = w;
    g.back_h = h;
}

void capture_inputs(VkCommandBuffer cmd, VkImage mv, VkImageLayout mv_layout, VkImage depth, VkImageLayout depth_layout, uint32_t w, uint32_t h, const float* consts) {
    if (!g.ok || !g.device) return;
    Slot& s = g.slots[g.slot];
    if (!ensure(s.mv, VK_FORMAT_R16G16_SFLOAT, w, h, VK_IMAGE_ASPECT_COLOR_BIT) || !ensure(s.depth, VK_FORMAT_D24_UNORM_S8_UINT, w, h, VK_IMAGE_ASPECT_DEPTH_BIT)) return;
    copy_into(cmd, mv, mv_layout, VK_IMAGE_ASPECT_COLOR_BIT, s.mv);
    copy_into(cmd, depth, depth_layout, VK_IMAGE_ASPECT_DEPTH_BIT, s.depth);
    std::memcpy(g.consts, consts, sizeof g.consts);
    g.inputs = true;
}

bool hudless_pending() { return g.ok && g.inputs && !g.hudless; }

void capture_hudless(VkCommandBuffer cmd, VkImage src, VkImageLayout layout, uint32_t w, uint32_t h) {
    if (!hudless_pending() || !g.back_w || !g.back_h) return;
    Slot& s = g.slots[g.slot];
    if (!ensure(s.hudless, VK_FORMAT_B8G8R8A8_UNORM, g.back_w, g.back_h, VK_IMAGE_ASPECT_COLOR_BIT)) return;
    if (layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) barrier(cmd, src, VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(cmd, s.hudless.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit b{};
    b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[1] = {int32_t(w), int32_t(h), 1};
    b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.dstOffsets[1] = {int32_t(g.back_w), int32_t(g.back_h), 1};
    vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.hudless.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    barrier(cmd, s.hudless.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) barrier(cmd, src, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout);
    g.hudless = true;
}

uint64_t presented_total() { return g.presented_total; }

bool generating() { return g.ok && g.fg_on; }

VkImage ui_begin(VkCommandBuffer cmd, uint32_t w, uint32_t h) {
    if (!g.ok || !g.device || !g.token) return VK_NULL_HANDLE;
    Slot& s = g.slots[g.slot];
    bool fresh = !s.ui.image || s.ui.w != w || s.ui.h != h;
    if (!ensure(s.ui, VK_FORMAT_B8G8R8A8_UNORM, w, h, VK_IMAGE_ASPECT_COLOR_BIT)) return VK_NULL_HANDLE;
    if (fresh) s.ui_ready = false;
    if (!s.ui_ready) {
        barrier(cmd, s.ui.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue zero{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, s.ui.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        s.ui_ready = true;
    } else {
        barrier(cmd, s.ui.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    }
    g.ui = true;
    return s.ui.image;
}

void pre_submit(VkCommandBuffer cmd, uint32_t back_w, uint32_t back_h) {
    if (!g.ok || !g.token) return;
    Slot& us = g.slots[g.slot];
    bool ui = g.ui && us.ui.w == back_w && us.ui.h == back_h;
    if (g.ui) barrier(cmd, us.ui.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    g.ui = false;
    set_backbuffer(back_w, back_h);
    static const bool tags_off = std::getenv("GOWA_DLSSG_NOTAGS") != nullptr;
    bool valid = g.inputs && !tags_off;
    slPCLSetMarker(sl::PCLMarker::eSimulationEnd, *g.token);
    slPCLSetMarker(sl::PCLMarker::eRenderSubmitStart, *g.token);
    sl::Constants c{};
    fill_constants(c, valid);
    if (SL_FAILED(res, slSetConstants(c, *g.token, sl::ViewportHandle(0)))) {
        static int n = 0;
        if (n++ < 5) LOG_WARN("dlssg", "slSetConstants failed %d", int(res));
    }
    Slot& s = g.slots[g.slot];
    sl::Resource depth = valid ? resource(s.depth) : sl::Resource{};
    sl::Resource mvec = valid ? resource(s.mv) : sl::Resource{};
    sl::Resource hud = (valid && g.hudless) ? resource(s.hudless) : sl::Resource{};
    sl::Extent ext{0, 0, s.mv.w, s.mv.h};
    sl::Resource uir = (valid && ui) ? resource(us.ui) : sl::Resource{};
    sl::Extent hext{0, 0, g.back_w, g.back_h};
    sl::ResourceTag tags[4] = {
        sl::ResourceTag(valid ? &depth : nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, valid ? &ext : nullptr),
        sl::ResourceTag(valid ? &mvec : nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, valid ? &ext : nullptr),
        sl::ResourceTag((valid && g.hudless) ? &hud : nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, (valid && g.hudless) ? &hext : nullptr),
        sl::ResourceTag((valid && ui) ? &uir : nullptr, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent, (valid && ui) ? &hext : nullptr),
    };
    if (SL_FAILED(res, slSetTagForFrame(*g.token, sl::ViewportHandle(0), tags, 4,reinterpret_cast<sl::CommandBuffer*>(cmd)))) {
        static int n = 0;
        if (n++ < 5) LOG_WARN("dlssg", "slSetTagForFrame failed %d", int(res));
    }
    static const bool force_off = std::getenv("GOWA_DLSSG_OFF") != nullptr;
    set_options(valid && !force_off);
    g.base_frames++;
    if (valid) g.fg_frames++;
    if (valid && g.hudless) g.hud_frames++;
}

void post_submit() {
    if (!g.ok || !g.token) return;
    slPCLSetMarker(sl::PCLMarker::eRenderSubmitEnd, *g.token);
}

void present_begin() {
    if (!g.ok || !g.token) return;
    slPCLSetMarker(sl::PCLMarker::ePresentStart, *g.token);
}

void present_end() {
    if (!g.ok) return;
    if (g.token) slPCLSetMarker(sl::PCLMarker::ePresentEnd, *g.token);
    sl::DLSSGState st{};
    if (SL_SUCCEEDED(res, slDLSSGGetState(sl::ViewportHandle(0), st, nullptr))) {
        uint64_t np = st.numFramesActuallyPresented ? st.numFramesActuallyPresented : 1;
        g.presented += np;
        g.presented_total += np;
        static sl::DLSSGStatus last = sl::DLSSGStatus::eOk;
        if (st.status != last) {
            LOG_WARN("dlssg", "DLSS-G status %u", uint32_t(st.status));
            last = st.status;
        }
    }
    auto now = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(now - g.stat_start).count();
    if (secs >= 5.0) {
        LOG_INFO("dlssg", "base %.1f fps, presented %.1f fps (x%.2f), fg frames %llu of %llu, hudless tagged %llu", g.base_frames / secs, g.presented / secs, g.base_frames ? double(g.presented) / g.base_frames : 0.0,
                 static_cast<unsigned long long>(g.fg_frames), static_cast<unsigned long long>(g.base_frames), static_cast<unsigned long long>(g.hud_frames));
        g.base_frames = g.presented = g.fg_frames = g.hud_frames = 0;
        g.stat_start = now;
    }
    g.prev_valid = g.inputs;
    g.inputs = false;
    g.hudless = false;
    g.slot ^= 1;
    g.frame++;
    next_token();
}

void shutdown() {
    if (!g.ok) return;
    set_options(false);
    slShutdown();
    g.ok = false;
}

}

#else

namespace gpu::dlssg {

bool requested() {
    static const bool r = [] {
        const char* v = std::getenv("GOWA_DLSSG");
        return v && std::atoi(v) != 0;
    }();
    return r;
}
bool load_vulkan() {
    if (requested()) LOG_WARN("dlssg", "GOWA_DLSSG set but this build has no Streamline SDK");
    return false;
}
bool active() { return false; }
void device_ready(VkPhysicalDevice, VkDevice) {}
bool want_low_latency_present() { return false; }
void set_backbuffer(uint32_t, uint32_t) {}
void capture_inputs(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, uint32_t, const float*) {}
bool hudless_pending() { return false; }
void capture_hudless(VkCommandBuffer, VkImage, VkImageLayout, uint32_t, uint32_t) {}
uint64_t presented_total() { return 0; }
bool generating() { return false; }
VkImage ui_begin(VkCommandBuffer, uint32_t, uint32_t) { return VK_NULL_HANDLE; }
void pre_submit(VkCommandBuffer, uint32_t, uint32_t) {}
void post_submit() {}
void present_begin() {}
void present_end() {}
void shutdown() {}

}

#endif
