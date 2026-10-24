#include "dlssnr.h"

#include "runtime/log.h"

#include <cstdlib>
#include <cstring>

#ifdef GOWA_HAVE_NGX

#include "shader_compiler.h"

#include <nvsdk_ngx_defs_vk.h>
#include <nvsdk_ngx_params.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace gpu::dlssnr {

namespace {

constexpr int kFeatureDlssNr = 18;
constexpr uint32_t kSdkVersion = 0x15;

using CallFn = unsigned int (*)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

class Params final : public NVSDK_NGX_Parameter {
public:
    enum Type { U64, F32, F64, U32, I32, PTR };
    struct Value {
        Type t = I32;
        unsigned long long u = 0;
        double d = 0.0;
        void* p = nullptr;
    };

    explicit Params(const char* tag) : tag_(tag) {}

    void log_sets(bool on) { log_sets_ = on; }

    void Set(const char* n, unsigned long long v) override { put(n, U64, v, double(v), nullptr); }
    void Set(const char* n, float v) override { put(n, F32, static_cast<unsigned long long>(static_cast<long long>(v)), v, nullptr); }
    void Set(const char* n, double v) override { put(n, F64, static_cast<unsigned long long>(static_cast<long long>(v)), v, nullptr); }
    void Set(const char* n, unsigned int v) override { put(n, U32, v, double(v), nullptr); }
    void Set(const char* n, int v) override { put(n, I32, static_cast<unsigned long long>(static_cast<long long>(v)), double(v), nullptr); }
    void Set(const char* n, ID3D11Resource* v) override { put(n, PTR, 0, 0.0, v); }
    void Set(const char* n, ID3D12Resource* v) override { put(n, PTR, 0, 0.0, v); }
    void Set(const char* n, void* v) override { put(n, PTR, 0, 0.0, v); }

    NVSDK_NGX_Result Get(const char* n, unsigned long long* o) const override {
        const Value* v = find(n);
        if (!v || v->t == PTR) return miss(n);
        *o = v->u;
        return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char* n, float* o) const override {
        const Value* v = find(n);
        if (!v || v->t == PTR) return miss(n);
        *o = static_cast<float>(v->d);
        return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char* n, double* o) const override {
        const Value* v = find(n);
        if (!v || v->t == PTR) return miss(n);
        *o = v->d;
        return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char* n, unsigned int* o) const override {
        const Value* v = find(n);
        if (!v || v->t == PTR) return miss(n);
        *o = static_cast<unsigned int>(v->u);
        return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char* n, int* o) const override {
        const Value* v = find(n);
        if (!v || v->t == PTR) return miss(n);
        *o = static_cast<int>(static_cast<long long>(v->u));
        return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char* n, ID3D11Resource** o) const override { return get_ptr(n, reinterpret_cast<void**>(o)); }
    NVSDK_NGX_Result Get(const char* n, ID3D12Resource** o) const override { return get_ptr(n, reinterpret_cast<void**>(o)); }
    NVSDK_NGX_Result Get(const char* n, void** o) const override { return get_ptr(n, o); }
    void Reset() override { values_.clear(); }

    const Value* find(const char* n) const {
        auto it = values_.find(n);
        return it == values_.end() ? nullptr : &it->second;
    }

private:
    void put(const char* n, Type t, unsigned long long u, double d, void* p) {
        Value& v = values_[n];
        v.t = t;
        v.u = u;
        v.d = d;
        v.p = p;
        if (log_sets_) {
            if (t == PTR) LOG_INFO("dlssnr", "%s set %s = %p", tag_, n, p);
            else LOG_INFO("dlssnr", "%s set %s = %g", tag_, n, d);
        }
    }
    NVSDK_NGX_Result get_ptr(const char* n, void** o) const {
        const Value* v = find(n);
        if (!v) return miss(n);
        *o = v->t == PTR ? v->p : reinterpret_cast<void*>(static_cast<uintptr_t>(v->u));
        return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result miss(const char* n) const {
        if (missed_.size() < 200 && missed_.insert(n).second) LOG_INFO("dlssnr", "%s query of unset %s", tag_, n);
        return NVSDK_NGX_Result_Fail;
    }

    const char* tag_;
    bool log_sets_ = false;
    std::map<std::string, Value> values_;
    mutable std::set<std::string> missed_;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t w = 0, h = 0;
};

struct Snippet {
    const char* tag = "";
    HMODULE dll = nullptr;
    void* init_ext2 = nullptr;
    void* populate = nullptr;
    void* create1 = nullptr;
    void* evaluate = nullptr;
    void* release = nullptr;
    void* shutdown1 = nullptr;
    Params* init_params = nullptr;
    Params* caps = nullptr;
    Params* create_params = nullptr;
    Params* eval_params = nullptr;
    NVSDK_NGX_Handle* handle = nullptr;
    uint32_t in_w = 0, in_h = 0, out_w = 0, out_h = 0;
    bool ok = false;
    bool prev = false;
};

struct Pipe {
    VkDescriptorSetLayout set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    bool tried = false;
};

enum class Up { Old, Dlss, Edge };

enum Pass { kPassNr, kPassUp, kPassCompose, kPassLift, kPassCount };

constexpr uint32_t kRing = 4;

struct State {
    bool tried = false;
    bool ok = false;
    bool failed = false;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    HMODULE bridge = nullptr;
    CallFn call = nullptr;
    Snippet nr, sr;
    Up up = Up::Old;
    Image hud, color, out, lifted, depth, mv, upimg, comp;
    NVSDK_NGX_Resource_VK r_color{}, r_out{}, r_depth{}, r_mv{}, s_color{}, s_out{}, s_depth{}, s_mv{};
    bool inputs = false;
    bool ran = false;
    bool prev_ran = false;
    bool composed = false;
    bool upscaled = false;
    uint32_t src_w = 0, src_h = 0;
    uint64_t frames = 0, evals = 0, fails = 0, total_evals = 0;
    VkQueryPool queries[kRing] = {};
    uint32_t written[kRing] = {};
    uint32_t ring = 0;
    double ts_period = 1.0;
    double pass_ms[kPassCount] = {};
    uint32_t pass_n[kPassCount] = {};
    Pipe compose_old, compose_new, edge, lift;
    VkSampler linear = VK_NULL_HANDLE;
    std::chrono::steady_clock::time_point stat_start;
    std::wstring data_dir;
};

State g;

double env_f(const char* n, double d) {
    const char* e = std::getenv(n);
    return e && *e ? std::atof(e) : d;
}

std::filesystem::path exe_dir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

std::filesystem::path find_dll(const char* name, const char* env) {
    if (const char* p = std::getenv(env)) return std::filesystem::path(p);
    if (const char* d = std::getenv("GOWA_DLSS_DIR")) return std::filesystem::path(d) / name;
    std::error_code ec;
    for (std::filesystem::path p = exe_dir(); !p.empty(); p = p.parent_path()) {
        if (std::filesystem::exists(p / "dlss" / name, ec)) return p / "dlss" / name;
        if (p == p.parent_path()) break;
    }
#ifdef GOWA_DLSS_DEFAULT_DIR
    return std::filesystem::path(GOWA_DLSS_DEFAULT_DIR) / name;
#else
    return exe_dir() / "dlss" / name;
#endif
}

template <typename... A>
unsigned int ngx(void* fn, A... a) {
    uintptr_t v[9] = {uintptr_t(a)...};
    return g.call(fn, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]);
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
    img.aspect = aspect;
    img.layout = VK_IMAGE_LAYOUT_UNDEFINED;
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
        LOG_ERROR("dlssnr", "cannot create %ux%u image (format %d)", w, h, int(format));
        destroy_image(img);
        return false;
    }
    return true;
}

void to(VkCommandBuffer cmd, Image& img, VkImageLayout layout) {
    if (img.layout == layout) {
        image_barrier(cmd, img.image, img.aspect, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        return;
    }
    image_barrier(cmd, img.image, img.aspect, img.layout, layout);
    img.layout = layout;
}

void blit(VkCommandBuffer cmd, VkImage src, uint32_t sw, uint32_t sh, VkImage dst, uint32_t dw, uint32_t dh, VkFilter filter) {
    VkImageBlit b{};
    b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[1] = {int32_t(sw), int32_t(sh), 1};
    b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.dstOffsets[1] = {int32_t(dw), int32_t(dh), 1};
    vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, filter);
}

void fill(NVSDK_NGX_Resource_VK& r, const Image& img, bool rw) {
    r = {};
    r.Resource.ImageViewInfo.ImageView = img.view;
    r.Resource.ImageViewInfo.Image = img.image;
    r.Resource.ImageViewInfo.SubresourceRange = {img.aspect, 0, 1, 0, 1};
    r.Resource.ImageViewInfo.Format = img.format;
    r.Resource.ImageViewInfo.Width = img.w;
    r.Resource.ImageViewInfo.Height = img.h;
    r.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
    r.ReadWrite = rw;
}

void apply_overrides(Params& p, const char* env) {
    const char* e = std::getenv(env);
    if (!e) return;
    std::string s = e;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(';', pos);
        if (end == std::string::npos) end = s.size();
        std::string kv = s.substr(pos, end - pos);
        size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
            if (v.find('.') != std::string::npos) p.Set(k.c_str(), static_cast<float>(std::atof(v.c_str())));
            else p.Set(k.c_str(), std::atoi(v.c_str()));
        }
        pos = end + 1;
    }
}

void set_tuning(Params& p) {
    p.Set("DLSSNR.Hint.Render.Preset", static_cast<int>(env_f("GOWA_DLSSNR_PRESET", 1)));
    p.Set("DLSSNR.Style", static_cast<int>(env_f("GOWA_DLSSNR_STYLE", 0)));
    p.Set("DLSSNR.Intensity", static_cast<float>(env_f("GOWA_DLSSNR_INTENSITY", 0.7)));
    p.Set("DLSSNR.LocalToneStrength", static_cast<float>(env_f("GOWA_DLSSNR_TONE", 0.25)));
    p.Set("DLSSNR.LocalStructureStrength", static_cast<float>(env_f("GOWA_DLSSNR_STRUCTURE", 0.35)));
    p.Set("DLSSNR.SkinStructureStrength", static_cast<float>(env_f("GOWA_DLSSNR_SKIN", 0.5)));
    p.Set("DLSSNR.UseAutoMask", static_cast<int>(env_f("GOWA_DLSSNR_AUTOMASK", 1)));
    p.Set("DLSSNR.UICorrection", static_cast<int>(env_f("GOWA_DLSSNR_UICORRECTION", 0)));
    p.Set("DLSSNR.DepthInverted", 0);
    apply_overrides(p, "GOWA_DLSSNR_PARAMS");
}

bool old_path() { return mode() == 2 && g.up == Up::Old; }

float scaling_ratio() { return old_path() ? static_cast<float>(env_f("GOWA_DLSSNR_SCALE", 0.5)) : 1.0f; }

float nr_scale() {
    if (old_path()) return 1.0f;
    static const float s = std::clamp(static_cast<float>(env_f("GOWA_DLSSNR_SCALE", 1.0)), 0.25f, 1.0f);
    return s;
}

void set_rect(Params& p, const char* base, uint32_t w, uint32_t h) {
    std::string b = base;
    p.Set((b + "SubrectBaseX").c_str(), 0u);
    p.Set((b + "SubrectBaseY").c_str(), 0u);
    p.Set((b + "SubrectWidth").c_str(), w);
    p.Set((b + "SubrectHeight").c_str(), h);
}

bool load_snippet(Snippet& s, const std::filesystem::path& dp) {
    s.dll = LoadLibraryW(dp.wstring().c_str());
    if (!s.dll) {
        LOG_ERROR("dlssnr", "cannot load %s (error %lu)", dp.string().c_str(), GetLastError());
        return false;
    }
    DWORD hnd = 0;
    DWORD sz = GetFileVersionInfoSizeW(dp.wstring().c_str(), &hnd);
    std::vector<uint8_t> buf(sz);
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (sz && GetFileVersionInfoW(dp.wstring().c_str(), 0, sz, buf.data()) && VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&fi), &len) && fi) {
        LOG_INFO("dlssnr", "loaded %s version %u.%u.%u.%u", dp.string().c_str(), HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    }
    s.init_ext2 = reinterpret_cast<void*>(GetProcAddress(s.dll, "NVSDK_NGX_VULKAN_Init_Ext2"));
    s.populate = reinterpret_cast<void*>(GetProcAddress(s.dll, "NVSDK_NGX_VULKAN_PopulateParameters_Impl"));
    s.create1 = reinterpret_cast<void*>(GetProcAddress(s.dll, "NVSDK_NGX_VULKAN_CreateFeature1"));
    s.evaluate = reinterpret_cast<void*>(GetProcAddress(s.dll, "NVSDK_NGX_VULKAN_EvaluateFeature"));
    s.release = reinterpret_cast<void*>(GetProcAddress(s.dll, "NVSDK_NGX_VULKAN_ReleaseFeature"));
    s.shutdown1 = reinterpret_cast<void*>(GetProcAddress(s.dll, "NVSDK_NGX_VULKAN_Shutdown1"));
    if (!s.init_ext2 || !s.populate || !s.create1 || !s.evaluate || !s.release) {
        LOG_ERROR("dlssnr", "%s lacks the Vulkan NGX snippet exports", dp.string().c_str());
        return false;
    }
    std::string t = s.tag;
    s.init_params = new Params(s.tag);
    s.caps = new Params(s.tag);
    s.create_params = new Params(s.tag);
    s.eval_params = new Params(s.tag);
    s.caps->log_sets(std::getenv("GOWA_DLSSNR_VERBOSE") != nullptr);
    uint64_t app_id = static_cast<uint64_t>(env_f("GOWA_DLSSNR_APPID", 0x5a7e0a));
    unsigned int r = ngx(s.init_ext2, app_id, g.data_dir.c_str(), g.instance, g.phys, g.device, vkGetInstanceProcAddr, vkGetDeviceProcAddr, kSdkVersion, s.init_params);
    if (r != NVSDK_NGX_Result_Success) {
        LOG_ERROR("dlssnr", "%s NVSDK_NGX_VULKAN_Init_Ext2 failed: 0x%08X", s.tag, r);
        return false;
    }
    r = ngx(s.populate, s.caps);
    LOG_INFO("dlssnr", "%s NGX snippet initialised, PopulateParameters 0x%08X", s.tag, r);
    s.ok = true;
    return true;
}

void release_feature(Snippet& s) {
    if (!s.handle) return;
    vkDeviceWaitIdle(g.device);
    ngx(s.release, s.handle);
    s.handle = nullptr;
}

bool create_nr(VkCommandBuffer cmd, uint32_t w, uint32_t h) {
    Snippet& s = g.nr;
    if (s.handle && s.out_w == w && s.out_h == h) return true;
    release_feature(s);
    Params& p = *s.create_params;
    p.Reset();
    p.Set("DLSSNR.Width", w);
    p.Set("DLSSNR.Height", h);
    p.Set("DLSSNR.ScalingRatio", scaling_ratio());
    p.Set("DLSS.Feature.Create.Flags", 0);
    set_tuning(p);
    auto t0 = std::chrono::steady_clock::now();
    unsigned int r = ngx(s.create1, g.device, cmd, kFeatureDlssNr, s.create_params, &s.handle);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (r != NVSDK_NGX_Result_Success || !s.handle) {
        LOG_ERROR("dlssnr", "CreateFeature(18) %ux%u ratio %.3f failed: 0x%08X", w, h, scaling_ratio(), r);
        s.handle = nullptr;
        g.failed = true;
        return false;
    }
    s.out_w = w;
    s.out_h = h;
    s.prev = false;
    LOG_INFO("dlssnr", "DLSSNR feature created: output %ux%u, scaling ratio %.3f, handle %p (%.0f ms)", w, h, scaling_ratio(), static_cast<void*>(s.handle), ms);
    return true;
}

bool create_sr(VkCommandBuffer cmd, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh) {
    Snippet& s = g.sr;
    if (s.handle && s.in_w == w && s.in_h == h && s.out_w == ow && s.out_h == oh) return true;
    release_feature(s);
    Params& p = *s.create_params;
    p.Reset();
    p.Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    p.Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    p.Set(NVSDK_NGX_Parameter_Width, w);
    p.Set(NVSDK_NGX_Parameter_Height, h);
    p.Set(NVSDK_NGX_Parameter_OutWidth, ow);
    p.Set(NVSDK_NGX_Parameter_OutHeight, oh);
    p.Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(env_f("GOWA_DLSS_QUALITY", NVSDK_NGX_PerfQuality_Value_MaxPerf)));
    int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (env_f("GOWA_DLSS_AUTOEXPOSURE", 0) != 0) flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    p.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);
    p.Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
    int preset = static_cast<int>(env_f("GOWA_DLSS_PRESET", NVSDK_NGX_DLSS_Hint_Render_Preset_K));
    for (const char* n : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality}) {
        p.Set(n, preset);
    }
    apply_overrides(p, "GOWA_DLSS_PARAMS");
    auto t0 = std::chrono::steady_clock::now();
    unsigned int r = ngx(s.create1, g.device, cmd, NVSDK_NGX_Feature_SuperSampling, s.create_params, &s.handle);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (r != NVSDK_NGX_Result_Success || !s.handle) {
        LOG_ERROR("dlssnr", "DLSS SR CreateFeature %ux%u -> %ux%u failed: 0x%08X, falling back to the edge upsample", w, h, ow, oh, r);
        s.handle = nullptr;
        s.ok = false;
        g.up = Up::Edge;
        return false;
    }
    s.in_w = w;
    s.in_h = h;
    s.out_w = ow;
    s.out_h = oh;
    s.prev = false;
    LOG_INFO("dlssnr", "DLSS SR feature created: %ux%u -> %ux%u, preset %d, flags 0x%x (%.0f ms)", w, h, ow, oh, preset, flags, ms);
    return true;
}

const char* kEdgeLib = R"(
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
float lanczos2(float x) {
    x = abs(x);
    if (x < 1e-4) return 1.0;
    if (x >= 2.0) return 0.0;
    float a = 3.14159265 * x;
    return 2.0 * sin(a) * sin(a * 0.5) / (a * a);
}
vec3 edge_up(sampler2D t, vec2 q, ivec2 n, float strength) {
    ivec2 b = ivec2(floor(q));
    vec2 f = q - vec2(b);
    vec3 c[16];
    float l[16];
    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 4; i++) {
            vec3 v = texelFetch(t, clamp(b + ivec2(i - 1, j - 1), ivec2(0), n - 1), 0).rgb;
            c[j * 4 + i] = v;
            l[j * 4 + i] = luma(v);
        }
    }
    float jxx = 0.0, jxy = 0.0, jyy = 0.0;
    for (int j = 1; j <= 2; j++) {
        for (int i = 1; i <= 2; i++) {
            float gx = l[j * 4 + i + 1] - l[j * 4 + i - 1];
            float gy = l[(j + 1) * 4 + i] - l[(j - 1) * 4 + i];
            float w = (i == 1 ? 1.0 - f.x : f.x) * (j == 1 ? 1.0 - f.y : f.y);
            jxx += w * gx * gx;
            jxy += w * gx * gy;
            jyy += w * gy * gy;
        }
    }
    float tr = jxx + jyy;
    float disc = sqrt(max(0.25 * tr * tr - (jxx * jyy - jxy * jxy), 0.0));
    float l1 = 0.5 * tr + disc, l2 = 0.5 * tr - disc;
    vec2 d = abs(jxy) > 1e-8 ? normalize(vec2(l1 - jyy, jxy)) : (jxx >= jyy ? vec2(1.0, 0.0) : vec2(0.0, 1.0));
    float coh = (l1 - l2) / (l1 + l2 + 1e-6);
    float e = strength * coh * smoothstep(0.0004, 0.01, l1);
    float across = 1.0 + 0.5 * e, along = 1.0 / (1.0 + 1.5 * e);
    vec2 p = vec2(-d.y, d.x);
    vec3 acc = vec3(0.0);
    float ws = 0.0;
    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 4; i++) {
            vec2 o = vec2(i - 1, j - 1) - f;
            float u = dot(o, d) * across, v = dot(o, p) * along;
            float w = lanczos2(sqrt(u * u + v * v));
            acc += w * c[j * 4 + i];
            ws += w;
        }
    }
    vec3 r = acc / max(ws, 1e-4);
    vec3 mn = min(min(c[5], c[6]), min(c[9], c[10]));
    vec3 mx = max(max(c[5], c[6]), max(c[9], c[10]));
    return clamp(r, mn, mx);
}
)";

const char* kComposeOld = R"(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D fin;
layout(set = 0, binding = 1) uniform sampler2D hud;
layout(set = 0, binding = 2) uniform sampler2D nr;
layout(set = 0, binding = 3, rgba8) uniform writeonly image2D dst;
layout(push_constant) uniform P { vec4 src; vec4 osz; } pc;
void main() {
    ivec2 c = ivec2(gl_GlobalInvocationID.xy);
    if (c.x >= int(pc.osz.x) || c.y >= int(pc.osz.y)) return;
    vec2 uv = (vec2(c) + 0.5) / pc.osz.xy;
    vec2 p = uv * pc.src.xy - 0.5;
    ivec2 b = ivec2(floor(p));
    ivec2 lim = ivec2(pc.src.xy) - 1;
    float m = 0.0;
    for (int dy = 0; dy <= 1; dy++) {
        for (int dx = 0; dx <= 1; dx++) {
            ivec2 t = clamp(b + ivec2(dx, dy), ivec2(0), lim);
            vec3 d = abs(texelFetch(fin, t, 0).rgb - texelFetch(hud, t, 0).rgb);
            m = max(m, max(d.r, max(d.g, d.b)));
        }
    }
    float k = smoothstep(pc.osz.z, pc.osz.z * 2.0, m);
    vec3 f = textureLod(fin, uv * pc.src.xy / pc.src.zw, 0.0).rgb;
    vec3 n = textureLod(nr, uv, 0.0).rgb;
    imageStore(dst, c, vec4(mix(n, f, k), 1.0));
}
)";

const char* kComposeNew = R"(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D fin;
layout(set = 0, binding = 1) uniform sampler2D base;
layout(set = 0, binding = 2) uniform sampler2D up;
layout(set = 0, binding = 3, rgba8) uniform writeonly image2D dst;
layout(push_constant) uniform P { vec4 src; vec4 osz; } pc;
#include_edge
void main() {
    ivec2 c = ivec2(gl_GlobalInvocationID.xy);
    if (c.x >= int(pc.osz.x) || c.y >= int(pc.osz.y)) return;
    vec2 q = (vec2(c) + 0.5) * pc.src.xy / pc.osz.xy - 0.5;
    ivec2 b = ivec2(floor(q));
    ivec2 n = ivec2(pc.src.xy);
    float m = 0.0;
    for (int dy = 0; dy <= 1; dy++) {
        for (int dx = 0; dx <= 1; dx++) {
            ivec2 t = clamp(b + ivec2(dx, dy), ivec2(0), n - 1);
            vec3 d = abs(texelFetch(fin, t, 0).rgb - texelFetch(base, t, 0).rgb);
            m = max(m, max(d.r, max(d.g, d.b)));
        }
    }
    float k = smoothstep(pc.osz.z, pc.osz.z * 2.0, m);
    vec3 u = texelFetch(up, c, 0).rgb;
    if (k > 0.0) u = mix(u, edge_up(fin, q, n, pc.osz.w), k);
    imageStore(dst, c, vec4(u, 1.0));
}
)";

const char* kEdge = R"(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, rgba8) uniform writeonly image2D dst;
layout(push_constant) uniform P { vec4 src; vec4 osz; } pc;
#include_edge
void main() {
    ivec2 c = ivec2(gl_GlobalInvocationID.xy);
    if (c.x >= int(pc.osz.x) || c.y >= int(pc.osz.y)) return;
    vec2 q = (vec2(c) + 0.5) * pc.src.xy / pc.osz.xy - 0.5;
    imageStore(dst, c, vec4(edge_up(src, q, ivec2(pc.src.xy), pc.osz.w), 1.0));
}
)";

const char* kLift = R"(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D ref;
layout(set = 0, binding = 1) uniform sampler2D proxy;
layout(set = 0, binding = 2) uniform sampler2D neural;
layout(set = 0, binding = 3, rgba8) uniform writeonly image2D dst;
layout(push_constant) uniform P { vec4 sz; vec4 lo; } pc;
const float kFloor = 1.0 / 1024.0;
vec3 dec(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }
vec3 enc(vec3 c) { return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2)); }
float lum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
void main() {
    ivec2 c = ivec2(gl_GlobalInvocationID.xy);
    if (c.x >= int(pc.sz.x) || c.y >= int(pc.sz.y)) return;
    ivec2 n = ivec2(pc.lo.xy);
    vec2 pos = (vec2(c) + 0.5) * pc.lo.xy / pc.sz.xy - 0.5;
    ivec2 b = ivec2(floor(pos));
    vec3 orig = dec(texelFetch(ref, c, 0).rgb);
    float gf = log2(max(lum(orig), kFloor));
    float ss = pc.lo.z, rs = pc.lo.w;
    vec3 je = vec3(0.0), se = vec3(0.0);
    float jw = 0.0, sw = 0.0;
    for (int dy = -1; dy <= 2; dy++) {
        for (int dx = -1; dx <= 2; dx++) {
            ivec2 t = b + ivec2(dx, dy);
            vec2 dd = vec2(t) - pos;
            ivec2 tc = clamp(t, ivec2(0), n - 1);
            vec3 pv = dec(texelFetch(proxy, tc, 0).rgb);
            vec3 nv = dec(texelFetch(neural, tc, 0).rgb);
            vec3 e = log2(max(nv, vec3(kFloor))) - log2(max(pv, vec3(kFloor)));
            float gd = gf - log2(max(lum(pv), kFloor));
            float ws = exp(-dot(dd, dd) / (2.0 * ss * ss));
            float wr = exp(-gd * gd / (2.0 * rs * rs));
            je += ws * wr * e;
            jw += ws * wr;
            se += ws * e;
            sw += ws;
        }
    }
    vec3 e = jw > 1e-6 ? je / jw : se / sw;
    vec3 gain = exp2(e);
    vec3 r = mix(orig + kFloor * (gain - 1.0), orig * gain, step(vec3(kFloor), orig));
    imageStore(dst, c, vec4(enc(r), 1.0));
}
)";

bool make_pipe(Pipe& p, const char* src, uint32_t samplers, uint32_t pc_size) {
    if (p.tried) return p.pipe != VK_NULL_HANDLE;
    p.tried = true;
    std::string text = src;
    size_t at = text.find("#include_edge");
    if (at != std::string::npos) text.replace(at, std::strlen("#include_edge"), kEdgeLib);
    std::vector<uint32_t> spirv;
    std::string log;
    if (!compile_glsl(ShaderStage::Compute, text.c_str(), spirv, log)) {
        LOG_ERROR("dlssnr", "compute shader failed: %s", log.c_str());
        return false;
    }
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = spirv.size() * 4;
    mi.pCode = spirv.data();
    VkShaderModule module;
    if (vkCreateShaderModule(g.device, &mi, nullptr, &module) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b[8]{};
    for (uint32_t i = 0; i <= samplers; i++) {
        b[i].binding = i;
        b[i].descriptorType = i < samplers ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dl.bindingCount = samplers + 1;
    dl.pBindings = b;
    if (vkCreateDescriptorSetLayout(g.device, &dl, nullptr, &p.set) != VK_SUCCESS) return false;
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, pc_size};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &p.set;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(g.device, &pl, nullptr, &p.layout) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
    ci.layout = p.layout;
    VkResult r = vkCreateComputePipelines(g.device, VK_NULL_HANDLE, 1, &ci, nullptr, &p.pipe);
    vkDestroyShaderModule(g.device, module, nullptr);
    if (r != VK_SUCCESS) {
        p.pipe = VK_NULL_HANDLE;
        return false;
    }
    if (!g.linear) {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0.0f;
        if (vkCreateSampler(g.device, &si, nullptr, &g.linear) != VK_SUCCESS) return false;
    }
    return true;
}

void dispatch(VkCommandBuffer cmd, Pipe& p, std::initializer_list<std::pair<VkImageView, VkImageLayout>> ins, VkImageView out, const float* pc, uint32_t pc_size, uint32_t w, uint32_t h) {
    VkDescriptorImageInfo ii[8]{};
    VkWriteDescriptorSet wr[8]{};
    uint32_t n = 0;
    for (auto& [v, l] : ins) {
        ii[n] = {g.linear, v, l};
        wr[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, n, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[n], nullptr, nullptr};
        n++;
    }
    ii[n] = {VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL};
    wr[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, n, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii[n], nullptr, nullptr};
    n++;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipe);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, n, wr);
    vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pc_size, pc);
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
}

void stamp(VkCommandBuffer cmd, Pass pass, bool end) {
    uint32_t q = pass * 2 + (end ? 1 : 0);
    vkCmdWriteTimestamp(cmd, end ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g.queries[g.ring], q);
    if (end) g.written[g.ring] |= 1u << pass;
}

void begin_timings(VkCommandBuffer cmd) {
    g.ring = (g.ring + 1) % kRing;
    uint32_t mask = g.written[g.ring];
    if (mask) {
        uint64_t ts[kPassCount * 2][2] = {};
        vkGetQueryPoolResults(g.device, g.queries[g.ring], 0, kPassCount * 2, sizeof ts, ts, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        for (uint32_t p = 0; p < kPassCount; p++) {
            if (!(mask & (1u << p)) || !ts[p * 2][1] || !ts[p * 2 + 1][1]) continue;
            g.pass_ms[p] += double(ts[p * 2 + 1][0] - ts[p * 2][0]) * g.ts_period * 1e-6;
            g.pass_n[p]++;
        }
    }
    g.written[g.ring] = 0;
    vkCmdResetQueryPool(cmd, g.queries[g.ring], 0, kPassCount * 2);
}

Up parse_up() {
    const char* e = std::getenv("GOWA_DLSSNR_UPSCALE");
    std::string s = e ? e : "dlss";
    if (s == "old") return Up::Old;
    if (s == "edge") return Up::Edge;
    return Up::Dlss;
}

bool run_nr(VkCommandBuffer cmd, Image& color, Image& out) {
    Snippet& s = g.nr;
    if (!create_nr(cmd, out.w, out.h)) return false;
    to(cmd, out, VK_IMAGE_LAYOUT_GENERAL);
    bool have_guides = g.inputs && g.mv.image && g.depth.image;
    fill(g.r_color, color, false);
    fill(g.r_out, out, true);
    Params& p = *s.eval_params;
    p.Reset();
    p.Set("DLSSNR.Color", static_cast<void*>(&g.r_color));
    p.Set("DLSSNR.Output", static_cast<void*>(&g.r_out));
    set_rect(p, "DLSSNR.Color", color.w, color.h);
    set_rect(p, "DLSSNR.Output", out.w, out.h);
    if (have_guides) {
        fill(g.r_depth, g.depth, false);
        fill(g.r_mv, g.mv, false);
        p.Set("DLSSNR.Depth", static_cast<void*>(&g.r_depth));
        p.Set("DLSSNR.MVec", static_cast<void*>(&g.r_mv));
        set_rect(p, "DLSSNR.Depth", g.depth.w, g.depth.h);
        set_rect(p, "DLSSNR.MVec", g.mv.w, g.mv.h);
        float ms = static_cast<float>(env_f("GOWA_DLSSNR_MVSCALE", 1.0)) * (old_path() ? 1.0f : float(color.w) / float(g.mv.w));
        p.Set("DLSSNR.MVecScaleX", ms);
        p.Set("DLSSNR.MVecScaleY", ms);
    }
    p.Set("DLSSNR.Width", out.w);
    p.Set("DLSSNR.Height", out.h);
    p.Set("DLSSNR.ScalingRatio", scaling_ratio());
    p.Set("DLSSNR.Reset", s.prev ? 0 : 1);
    set_tuning(p);
    stamp(cmd, kPassNr, false);
    unsigned int r = ngx(s.evaluate, cmd, s.handle, s.eval_params, nullptr);
    stamp(cmd, kPassNr, true);
    out.layout = VK_IMAGE_LAYOUT_GENERAL;
    if (r != NVSDK_NGX_Result_Success) {
        if (g.fails++ < 5) LOG_ERROR("dlssnr", "EvaluateFeature failed: 0x%08X", r);
        s.prev = false;
        return false;
    }
    if (g.total_evals++ == 0) LOG_INFO("dlssnr", "first evaluate ok: color %ux%u -> output %ux%u, guides %s", color.w, color.h, out.w, out.h, have_guides ? "depth+mv" : "none");
    s.prev = true;
    return true;
}

bool run_sr(VkCommandBuffer cmd, Image& color, Image& out) {
    Snippet& s = g.sr;
    if (!g.inputs || !g.mv.image || !g.depth.image) return false;
    if (!create_sr(cmd, color.w, color.h, out.w, out.h)) return false;
    to(cmd, color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    to(cmd, out, VK_IMAGE_LAYOUT_GENERAL);
    fill(g.s_color, color, false);
    fill(g.s_out, out, true);
    fill(g.s_depth, g.depth, false);
    fill(g.s_mv, g.mv, false);
    Params& p = *s.eval_params;
    p.Reset();
    p.Set(NVSDK_NGX_Parameter_Color, static_cast<void*>(&g.s_color));
    p.Set(NVSDK_NGX_Parameter_Output, static_cast<void*>(&g.s_out));
    p.Set(NVSDK_NGX_Parameter_Depth, static_cast<void*>(&g.s_depth));
    p.Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<void*>(&g.s_mv));
    p.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
    p.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
    p.Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
    p.Set(NVSDK_NGX_Parameter_Reset, s.prev ? 0 : 1);
    float ms = float(color.w) / float(g.mv.w);
    p.Set(NVSDK_NGX_Parameter_MV_Scale_X, ms);
    p.Set(NVSDK_NGX_Parameter_MV_Scale_Y, ms);
    p.Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, 0u);
    p.Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, 0u);
    p.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 0u);
    p.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, 0u);
    p.Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0u);
    p.Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0u);
    p.Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0u);
    p.Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0u);
    p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, color.w);
    p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, color.h);
    p.Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
    p.Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
    p.Set(NVSDK_NGX_Parameter_DLSS_Indicator_Invert_X_Axis, 0);
    p.Set(NVSDK_NGX_Parameter_DLSS_Indicator_Invert_Y_Axis, 0);
    apply_overrides(p, "GOWA_DLSS_EVAL_PARAMS");
    stamp(cmd, kPassUp, false);
    unsigned int r = ngx(s.evaluate, cmd, s.handle, s.eval_params, nullptr);
    stamp(cmd, kPassUp, true);
    out.layout = VK_IMAGE_LAYOUT_GENERAL;
    if (r != NVSDK_NGX_Result_Success) {
        static int n = 0;
        if (n++ < 5) LOG_ERROR("dlssnr", "DLSS SR EvaluateFeature failed: 0x%08X", r);
        s.prev = false;
        return false;
    }
    static bool first = true;
    if (first) LOG_INFO("dlssnr", "first DLSS SR evaluate ok: %ux%u -> %ux%u", color.w, color.h, out.w, out.h);
    first = false;
    s.prev = true;
    return true;
}

bool run_edge(VkCommandBuffer cmd, Image& src, Image& out) {
    if (!make_pipe(g.edge, kEdge, 1, 32)) return false;
    to(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    to(cmd, out, VK_IMAGE_LAYOUT_GENERAL);
    static const float strength = static_cast<float>(env_f("GOWA_DLSSNR_EDGE", 1.0));
    float pc[8] = {float(src.w), float(src.h), 0.0f, 0.0f, float(out.w), float(out.h), 0.0f, strength};
    stamp(cmd, kPassUp, false);
    dispatch(cmd, g.edge, {{src.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}, out.view, pc, sizeof pc, out.w, out.h);
    stamp(cmd, kPassUp, true);
    return true;
}

bool run_lift(VkCommandBuffer cmd, Image& ref, Image& proxy, Image& neural, Image& out) {
    if (!make_pipe(g.lift, kLift, 3, 32)) return false;
    to(cmd, ref, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    to(cmd, proxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    to(cmd, neural, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    to(cmd, out, VK_IMAGE_LAYOUT_GENERAL);
    float pc[8] = {float(out.w), float(out.h), 0.0f, 0.0f, float(proxy.w), float(proxy.h), static_cast<float>(env_f("GOWA_DLSSNR_LIFT_SPATIAL", 0.5)), static_cast<float>(env_f("GOWA_DLSSNR_LIFT_RANGE", 0.5))};
    stamp(cmd, kPassLift, false);
    dispatch(cmd, g.lift, {{ref.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {proxy.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {neural.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}, out.view, pc, sizeof pc, out.w,
             out.h);
    stamp(cmd, kPassLift, true);
    return true;
}

void restore(VkCommandBuffer cmd, VkImage src, VkImageLayout layout) {
    if (layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) image_barrier(cmd, src, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout);
}

bool evaluate_old(VkCommandBuffer cmd, VkImage src, VkImageLayout layout, uint32_t w, uint32_t h) {
    uint32_t ow = w * 2, oh = h * 2;
    if (!ensure(g.hud, VK_FORMAT_R8G8B8A8_UNORM, w, h, VK_IMAGE_ASPECT_COLOR_BIT) || !ensure(g.out, VK_FORMAT_R8G8B8A8_UNORM, ow, oh, VK_IMAGE_ASPECT_COLOR_BIT) ||
        !ensure(g.color, VK_FORMAT_R8G8B8A8_UNORM, ow, oh, VK_IMAGE_ASPECT_COLOR_BIT)) {
        return false;
    }
    if (layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) image_barrier(cmd, src, VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    to(cmd, g.hud, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    blit(cmd, src, w, h, g.hud.image, w, h, VK_FILTER_NEAREST);
    to(cmd, g.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    blit(cmd, src, w, h, g.color.image, ow, oh, VK_FILTER_LINEAR);
    to(cmd, g.color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    to(cmd, g.hud, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    bool ok = run_nr(cmd, g.color, g.out);
    if (ok) to(cmd, g.out, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    restore(cmd, src, layout);
    return ok;
}

bool evaluate_new(VkCommandBuffer cmd, VkImage src, VkImageLayout layout, uint32_t w, uint32_t h) {
    float sc = nr_scale();
    uint32_t sw = sc < 1.0f ? std::max(1u, uint32_t(w * sc + 0.5f)) : w, sh = sc < 1.0f ? std::max(1u, uint32_t(h * sc + 0.5f)) : h;
    if (!ensure(g.hud, VK_FORMAT_R8G8B8A8_UNORM, w, h, VK_IMAGE_ASPECT_COLOR_BIT) || !ensure(g.out, VK_FORMAT_R8G8B8A8_UNORM, sw, sh, VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    if (sc < 1.0f && (!ensure(g.color, VK_FORMAT_R8G8B8A8_UNORM, sw, sh, VK_IMAGE_ASPECT_COLOR_BIT) || !ensure(g.lifted, VK_FORMAT_R8G8B8A8_UNORM, w, h, VK_IMAGE_ASPECT_COLOR_BIT))) return false;
    if (layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) image_barrier(cmd, src, VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    to(cmd, g.hud, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    blit(cmd, src, w, h, g.hud.image, w, h, VK_FILTER_NEAREST);
    Image* in = &g.hud;
    if (sc < 1.0f) {
        to(cmd, g.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        blit(cmd, src, w, h, g.color.image, sw, sh, VK_FILTER_LINEAR);
        in = &g.color;
    }
    to(cmd, *in, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    to(cmd, g.hud, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!run_nr(cmd, *in, g.out)) {
        restore(cmd, src, layout);
        return false;
    }
    Image* base = &g.out;
    if (sc < 1.0f) {
        if (!run_lift(cmd, g.hud, g.color, g.out, g.lifted)) {
            restore(cmd, src, layout);
            return false;
        }
        base = &g.lifted;
    }
    to(cmd, *base, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    image_barrier(cmd, src, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    blit(cmd, base->image, w, h, src, w, h, VK_FILTER_NEAREST);
    image_barrier(cmd, src, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layout);
    g.upscaled = false;
    if (mode() == 2) {
        if (!ensure(g.upimg, VK_FORMAT_R8G8B8A8_UNORM, w * 2, h * 2, VK_IMAGE_ASPECT_COLOR_BIT)) return true;
        bool ok = false;
        if (g.up == Up::Dlss) ok = run_sr(cmd, *base, g.upimg);
        if (!ok) ok = run_edge(cmd, *base, g.upimg);
        if (ok) {
            to(cmd, g.upimg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            g.upscaled = true;
        }
    }
    to(cmd, *base, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

Image& base_image() { return nr_scale() < 1.0f ? g.lifted : g.out; }

}

int mode() {
    static const int m = [] {
        const char* v = std::getenv("GOWA_DLSSNR");
        int r = v ? std::atoi(v) : 0;
        return r == 1 || r == 2 ? r : 0;
    }();
    return m;
}

bool requested() { return mode() != 0; }

bool active() { return g.ok && !g.failed; }

void device_ready(VkInstance instance, VkPhysicalDevice phys, VkDevice device) {
    if (g.tried || !requested()) return;
    g.tried = true;
    g.instance = instance;
    g.phys = phys;
    g.device = device;
    g.up = mode() == 2 ? parse_up() : Up::Edge;
    std::filesystem::path bp = exe_dir() / "gowa_nvngx.dll";
    g.bridge = LoadLibraryW(bp.wstring().c_str());
    if (!g.bridge) {
        LOG_ERROR("dlssnr", "cannot load %s (error %lu)", bp.string().c_str(), GetLastError());
        return;
    }
    g.call = reinterpret_cast<CallFn>(GetProcAddress(g.bridge, "gowa_ngx_call"));
    if (!g.call) return;
    const char* ld = std::getenv("GOWA_SNAP_DIR");
    g.data_dir = ld ? std::filesystem::path(ld).wstring() : std::filesystem::current_path().wstring();
    g.nr.tag = "nr";
    g.sr.tag = "sr";
    if (!load_snippet(g.nr, find_dll("nvngx_dlssnr.dll", "GOWA_DLSSNR_DLL"))) return;
    if (g.up == Up::Dlss && !load_snippet(g.sr, find_dll("nvngx_dlss.dll", "GOWA_DLSS_DLL"))) {
        LOG_WARN("dlssnr", "DLSS SR unavailable, using the edge upsample");
        g.up = Up::Edge;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    g.ts_period = props.limits.timestampPeriod;
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = kPassCount * 2;
    for (auto& q : g.queries) vkCreateQueryPool(device, &qi, nullptr, &q);
    g.stat_start = std::chrono::steady_clock::now();
    g.ok = true;
    static const char* names[] = {"old", "dlss", "edge"};
    LOG_INFO("dlssnr", "DLSS neural rendering mode %dx ready (upscale %s, NR scale %.2f)", mode(), mode() == 2 ? names[int(g.up)] : "none", nr_scale());
}

void capture_inputs(VkCommandBuffer cmd, VkImage mv, VkImageLayout mv_layout, VkImage depth, VkImageLayout depth_layout, uint32_t w, uint32_t h) {
    if (!active()) return;
    if (!ensure(g.mv, VK_FORMAT_R16G16_SFLOAT, w, h, VK_IMAGE_ASPECT_COLOR_BIT) || !ensure(g.depth, VK_FORMAT_D24_UNORM_S8_UINT, w, h, VK_IMAGE_ASPECT_DEPTH_BIT)) return;
    VkImage srcs[2] = {mv, depth};
    VkImageLayout layouts[2] = {mv_layout, depth_layout};
    Image* dsts[2] = {&g.mv, &g.depth};
    for (int i = 0; i < 2; i++) {
        VkImageAspectFlags a = dsts[i]->aspect;
        if (layouts[i] != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) image_barrier(cmd, srcs[i], a, layouts[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        to(cmd, *dsts[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy c{};
        c.srcSubresource = {a, 0, 0, 1};
        c.dstSubresource = {a, 0, 0, 1};
        c.extent = {w, h, 1};
        vkCmdCopyImage(cmd, srcs[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dsts[i]->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        to(cmd, *dsts[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (layouts[i] != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) image_barrier(cmd, srcs[i], a, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layouts[i]);
    }
    g.inputs = true;
}

bool pending() { return active() && !g.ran; }

bool evaluate(VkCommandBuffer cmd, VkImage src, VkImageLayout layout, uint32_t w, uint32_t h) {
    if (!pending() || !w || !h) return false;
    g.ran = true;
    begin_timings(cmd);
    bool ok = old_path() ? evaluate_old(cmd, src, layout, w, h) : evaluate_new(cmd, src, layout, w, h);
    if (!ok) {
        g.prev_ran = false;
        return false;
    }
    g.evals++;
    g.prev_ran = true;
    g.src_w = w;
    g.src_h = h;
    return true;
}

bool output(VkImage& image, VkImageLayout& layout, uint32_t& w, uint32_t& h) {
    if (!active() || !g.ran || !g.prev_ran || mode() != 2) return false;
    Image& o = old_path() ? g.out : g.upimg;
    if (!old_path() && !g.upscaled) return false;
    image = o.image;
    layout = o.layout;
    w = o.w;
    h = o.h;
    return true;
}

bool compose(VkCommandBuffer cmd, VkImageView final_view, VkImageLayout final_layout, uint32_t w, uint32_t h, VkImage& image, VkImageLayout*& layout, uint32_t& ow, uint32_t& oh) {
    if (!active() || mode() != 2 || !g.ran || !g.prev_ran) return false;
    bool old = old_path();
    if (!old && !g.upscaled) return false;
    if (!(old ? make_pipe(g.compose_old, kComposeOld, 3, 32) : make_pipe(g.compose_new, kComposeNew, 3, 32))) return false;
    Image& big = old ? g.out : g.upimg;
    Image& ref = old ? g.hud : base_image();
    if (!ensure(g.comp, VK_FORMAT_R8G8B8A8_UNORM, big.w, big.h, VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    to(cmd, g.comp, VK_IMAGE_LAYOUT_GENERAL);
    to(cmd, big, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    to(cmd, ref, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    static const float thr = static_cast<float>(env_f("GOWA_DLSSNR_HUD_THRESHOLD", old ? 3.0 / 255.0 : 1.5 / 255.0));
    static const float strength = static_cast<float>(env_f("GOWA_DLSSNR_EDGE", 1.0));
    float pc[8] = {float(g.src_w), float(g.src_h), float(w), float(h), float(g.comp.w), float(g.comp.h), thr, strength};
    stamp(cmd, kPassCompose, false);
    dispatch(cmd, old ? g.compose_old : g.compose_new, {{final_view, final_layout}, {ref.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {big.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}, g.comp.view, pc, sizeof pc,
             g.comp.w, g.comp.h);
    stamp(cmd, kPassCompose, true);
    to(cmd, g.comp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    image = g.comp.image;
    layout = &g.comp.layout;
    ow = g.comp.w;
    oh = g.comp.h;
    g.composed = true;
    return true;
}

bool snapshot_images(VkCommandBuffer cmd, VkImage& in, uint32_t& w, uint32_t& h, VkImage& out, uint32_t& ow, uint32_t& oh) {
    if (!active() || !g.ran || !g.prev_ran || !g.hud.image) return false;
    Image& o = old_path() ? g.out : (g.upscaled ? g.upimg : base_image());
    if (!o.image) return false;
    to(cmd, g.hud, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    to(cmd, o, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    in = g.hud.image;
    w = g.hud.w;
    h = g.hud.h;
    out = o.image;
    ow = o.w;
    oh = o.h;
    return true;
}

void end_frame() {
    if (!g.ok) return;
    g.frames++;
    if (!g.ran) {
        g.prev_ran = false;
        g.nr.prev = false;
        g.sr.prev = false;
    }
    auto now = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(now - g.stat_start).count();
    if (secs >= 5.0) {
        auto avg = [](int p) { return g.pass_n[p] ? g.pass_ms[p] / g.pass_n[p] : 0.0; };
        LOG_INFO("dlssnr", "%.1f fps, NR evaluated %llu of %llu frames, GPU ms: NR %.2f lift %.2f upscale %.2f compose %.2f, failures %llu", g.frames / secs, static_cast<unsigned long long>(g.evals),
                 static_cast<unsigned long long>(g.frames), avg(kPassNr), avg(kPassLift), avg(kPassUp), avg(kPassCompose), static_cast<unsigned long long>(g.fails));
        g.frames = g.evals = 0;
        for (int p = 0; p < kPassCount; p++) {
            g.pass_ms[p] = 0.0;
            g.pass_n[p] = 0;
        }
        g.stat_start = now;
    }
    g.ran = false;
    g.inputs = false;
    g.composed = false;
    g.upscaled = false;
}

void shutdown() {
    if (!g.ok) return;
    vkDeviceWaitIdle(g.device);
    for (Snippet* s : {&g.nr, &g.sr}) {
        if (!s->ok) continue;
        if (s->handle) ngx(s->release, s->handle);
        s->handle = nullptr;
        if (s->shutdown1) ngx(s->shutdown1, g.device);
    }
    g.ok = false;
}

}

#else

namespace gpu::dlssnr {

int mode() {
    static const int m = [] {
        const char* v = std::getenv("GOWA_DLSSNR");
        int r = v ? std::atoi(v) : 0;
        return r == 1 || r == 2 ? r : 0;
    }();
    return m;
}
bool requested() { return mode() != 0; }
bool active() { return false; }
void device_ready(VkInstance, VkPhysicalDevice, VkDevice) {
    if (requested()) LOG_WARN("dlssnr", "GOWA_DLSSNR set but this build has no NGX SDK headers");
}
void capture_inputs(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, uint32_t) {}
bool pending() { return false; }
bool evaluate(VkCommandBuffer, VkImage, VkImageLayout, uint32_t, uint32_t) { return false; }
bool output(VkImage&, VkImageLayout&, uint32_t&, uint32_t&) { return false; }
bool snapshot_images(VkCommandBuffer, VkImage&, uint32_t&, uint32_t&, VkImage&, uint32_t&, uint32_t&) { return false; }
bool compose(VkCommandBuffer, VkImageView, VkImageLayout, uint32_t, uint32_t, VkImage&, VkImageLayout*&, uint32_t&, uint32_t&) { return false; }
void end_frame() {}
void shutdown() {}

}

#endif
