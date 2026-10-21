#include "dlssnr.h"

#include "runtime/log.h"

#include <cstdlib>
#include <cstring>

#ifdef GOWA_HAVE_NGX

#include "shader_compiler.h"

#include <nvsdk_ngx_defs_vk.h>
#include <nvsdk_ngx_params.h>

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

struct State {
    bool tried = false;
    bool ok = false;
    bool failed = false;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    HMODULE bridge = nullptr;
    HMODULE dll = nullptr;
    CallFn call = nullptr;
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
    uint32_t feature_w = 0, feature_h = 0;
    Image hud, color, out, depth, mv, comp;
    NVSDK_NGX_Resource_VK r_color{}, r_out{}, r_depth{}, r_mv{};
    bool inputs = false;
    bool ran = false;
    bool prev_ran = false;
    bool composed = false;
    uint32_t src_w = 0, src_h = 0;
    uint64_t frames = 0, evals = 0, fails = 0, total_evals = 0;
    VkQueryPool queries = VK_NULL_HANDLE;
    bool query_pending = false;
    double ts_period = 1.0;
    double gpu_ms = 0.0;
    uint32_t gpu_n = 0;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkSampler linear = VK_NULL_HANDLE;
    bool pipe_tried = false;
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

std::filesystem::path find_dll() {
    if (const char* p = std::getenv("GOWA_DLSSNR_DLL")) return std::filesystem::path(p);
    if (const char* d = std::getenv("GOWA_DLSS_DIR")) return std::filesystem::path(d) / "nvngx_dlssnr.dll";
    std::error_code ec;
    for (std::filesystem::path p = exe_dir(); !p.empty(); p = p.parent_path()) {
        if (std::filesystem::exists(p / "dlss" / "nvngx_dlssnr.dll", ec)) return p / "dlss" / "nvngx_dlssnr.dll";
        if (p == p.parent_path()) break;
    }
#ifdef GOWA_DLSS_DEFAULT_DIR
    return std::filesystem::path(GOWA_DLSS_DEFAULT_DIR) / "nvngx_dlssnr.dll";
#else
    return exe_dir() / "dlss" / "nvngx_dlssnr.dll";
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

void apply_overrides(Params& p) {
    const char* e = std::getenv("GOWA_DLSSNR_PARAMS");
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
    apply_overrides(p);
}

float scaling_ratio() { return static_cast<float>(env_f("GOWA_DLSSNR_SCALE", mode() == 2 ? 0.5 : 1.0)); }

void set_rect(Params& p, const char* base, uint32_t w, uint32_t h) {
    std::string b = base;
    p.Set((b + "SubrectBaseX").c_str(), 0u);
    p.Set((b + "SubrectBaseY").c_str(), 0u);
    p.Set((b + "SubrectWidth").c_str(), w);
    p.Set((b + "SubrectHeight").c_str(), h);
}

bool create_feature(VkCommandBuffer cmd, uint32_t w, uint32_t h) {
    if (g.handle && g.feature_w == w && g.feature_h == h) return true;
    if (g.handle) {
        vkDeviceWaitIdle(g.device);
        ngx(g.release, g.handle);
        g.handle = nullptr;
    }
    Params& p = *g.create_params;
    p.Reset();
    p.Set("DLSSNR.Width", w);
    p.Set("DLSSNR.Height", h);
    p.Set("DLSSNR.ScalingRatio", scaling_ratio());
    p.Set("DLSS.Feature.Create.Flags", 0);
    set_tuning(p);
    auto t0 = std::chrono::steady_clock::now();
    unsigned int r = ngx(g.create1, g.device, cmd, kFeatureDlssNr, g.create_params, &g.handle);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (r != NVSDK_NGX_Result_Success || !g.handle) {
        LOG_ERROR("dlssnr", "CreateFeature(18) %ux%u ratio %.3f failed: 0x%08X", w, h, scaling_ratio(), r);
        g.handle = nullptr;
        g.failed = true;
        return false;
    }
    g.feature_w = w;
    g.feature_h = h;
    g.prev_ran = false;
    LOG_INFO("dlssnr", "DLSSNR feature created: output %ux%u, scaling ratio %.3f, handle %p (%.0f ms)", w, h, scaling_ratio(), static_cast<void*>(g.handle), ms);
    return true;
}

const char* kCompose = R"(#version 450
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

bool init_compose() {
    if (g.pipe_tried) return g.pipeline != VK_NULL_HANDLE;
    g.pipe_tried = true;
    std::vector<uint32_t> spirv;
    std::string log;
    if (!compile_glsl(ShaderStage::Compute, kCompose, spirv, log)) {
        LOG_ERROR("dlssnr", "compose shader failed: %s", log.c_str());
        return false;
    }
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = spirv.size() * 4;
    mi.pCode = spirv.data();
    VkShaderModule module;
    if (vkCreateShaderModule(g.device, &mi, nullptr, &module) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b[4]{};
    for (uint32_t i = 0; i < 4; i++) {
        b[i].binding = i;
        b[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dl.bindingCount = 4;
    dl.pBindings = b;
    if (vkCreateDescriptorSetLayout(g.device, &dl, nullptr, &g.set_layout) != VK_SUCCESS) return false;
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g.set_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(g.device, &pl, nullptr, &g.pipe_layout) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
    ci.layout = g.pipe_layout;
    VkResult r = vkCreateComputePipelines(g.device, VK_NULL_HANDLE, 1, &ci, nullptr, &g.pipeline);
    vkDestroyShaderModule(g.device, module, nullptr);
    if (r != VK_SUCCESS) {
        g.pipeline = VK_NULL_HANDLE;
        return false;
    }
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    return vkCreateSampler(g.device, &si, nullptr, &g.linear) == VK_SUCCESS;
}

void read_timings() {
    if (!g.query_pending) return;
    uint64_t ts[2] = {};
    if (vkGetQueryPoolResults(g.device, g.queries, 0, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        g.gpu_ms += double(ts[1] - ts[0]) * g.ts_period * 1e-6;
        g.gpu_n++;
    }
    g.query_pending = false;
}

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
    std::filesystem::path bp = exe_dir() / "gowa_nvngx.dll";
    g.bridge = LoadLibraryW(bp.wstring().c_str());
    if (!g.bridge) {
        LOG_ERROR("dlssnr", "cannot load %s (error %lu)", bp.string().c_str(), GetLastError());
        return;
    }
    g.call = reinterpret_cast<CallFn>(GetProcAddress(g.bridge, "gowa_ngx_call"));
    std::filesystem::path dp = find_dll();
    g.dll = LoadLibraryW(dp.wstring().c_str());
    if (!g.dll || !g.call) {
        LOG_ERROR("dlssnr", "cannot load %s (error %lu)", dp.string().c_str(), GetLastError());
        return;
    }
    {
        DWORD hnd = 0;
        DWORD sz = GetFileVersionInfoSizeW(dp.wstring().c_str(), &hnd);
        std::vector<uint8_t> buf(sz);
        VS_FIXEDFILEINFO* fi = nullptr;
        UINT len = 0;
        if (sz && GetFileVersionInfoW(dp.wstring().c_str(), 0, sz, buf.data()) && VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&fi), &len) && fi) {
            LOG_INFO("dlssnr", "loaded %s version %u.%u.%u.%u", dp.string().c_str(), HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
        }
    }
    g.init_ext2 = reinterpret_cast<void*>(GetProcAddress(g.dll, "NVSDK_NGX_VULKAN_Init_Ext2"));
    g.populate = reinterpret_cast<void*>(GetProcAddress(g.dll, "NVSDK_NGX_VULKAN_PopulateParameters_Impl"));
    g.create1 = reinterpret_cast<void*>(GetProcAddress(g.dll, "NVSDK_NGX_VULKAN_CreateFeature1"));
    g.evaluate = reinterpret_cast<void*>(GetProcAddress(g.dll, "NVSDK_NGX_VULKAN_EvaluateFeature"));
    g.release = reinterpret_cast<void*>(GetProcAddress(g.dll, "NVSDK_NGX_VULKAN_ReleaseFeature"));
    g.shutdown1 = reinterpret_cast<void*>(GetProcAddress(g.dll, "NVSDK_NGX_VULKAN_Shutdown1"));
    if (!g.init_ext2 || !g.populate || !g.create1 || !g.evaluate || !g.release) {
        LOG_ERROR("dlssnr", "%s lacks the Vulkan NGX snippet exports", dp.string().c_str());
        return;
    }
    g.init_params = new Params("init");
    g.caps = new Params("caps");
    g.create_params = new Params("create");
    g.eval_params = new Params("eval");
    g.caps->log_sets(std::getenv("GOWA_DLSSNR_VERBOSE") != nullptr);
    const char* ld = std::getenv("GOWA_SNAP_DIR");
    g.data_dir = ld ? std::filesystem::path(ld).wstring() : std::filesystem::current_path().wstring();
    uint64_t app_id = static_cast<uint64_t>(env_f("GOWA_DLSSNR_APPID", 0x5a7e0a));
    unsigned int r = ngx(g.init_ext2, app_id, g.data_dir.c_str(), instance, phys, device, vkGetInstanceProcAddr, vkGetDeviceProcAddr, kSdkVersion, g.init_params);
    if (r != NVSDK_NGX_Result_Success) {
        LOG_ERROR("dlssnr", "NVSDK_NGX_VULKAN_Init_Ext2 failed: 0x%08X", r);
        return;
    }
    r = ngx(g.populate, g.caps);
    LOG_INFO("dlssnr", "NGX snippet initialised, PopulateParameters 0x%08X", r);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    g.ts_period = props.limits.timestampPeriod;
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = 2;
    vkCreateQueryPool(device, &qi, nullptr, &g.queries);
    g.stat_start = std::chrono::steady_clock::now();
    g.ok = true;
    LOG_INFO("dlssnr", "DLSS neural rendering mode %dx ready (scaling ratio %.3f)", mode(), scaling_ratio());
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
    uint32_t ow = mode() == 2 ? w * 2 : w, oh = mode() == 2 ? h * 2 : h;
    if (!ensure(g.hud, VK_FORMAT_R8G8B8A8_UNORM, w, h, VK_IMAGE_ASPECT_COLOR_BIT) || !ensure(g.out, VK_FORMAT_R8G8B8A8_UNORM, ow, oh, VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    if (mode() == 2 && !ensure(g.color, VK_FORMAT_R8G8B8A8_UNORM, ow, oh, VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    Image& color = mode() == 2 ? g.color : g.hud;
    if (!create_feature(cmd, ow, oh)) return false;
    read_timings();
    if (layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) image_barrier(cmd, src, VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    to(cmd, g.hud, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    blit(cmd, src, w, h, g.hud.image, w, h, VK_FILTER_NEAREST);
    if (mode() == 2) {
        to(cmd, g.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        blit(cmd, src, w, h, g.color.image, ow, oh, VK_FILTER_LINEAR);
        to(cmd, g.color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    to(cmd, g.hud, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    to(cmd, g.out, VK_IMAGE_LAYOUT_GENERAL);
    bool have_guides = g.inputs && g.mv.image && g.depth.image;
    fill(g.r_color, color, false);
    fill(g.r_out, g.out, true);
    Params& p = *g.eval_params;
    p.Reset();
    p.Set("DLSSNR.Color", static_cast<void*>(&g.r_color));
    p.Set("DLSSNR.Output", static_cast<void*>(&g.r_out));
    set_rect(p, "DLSSNR.Color", color.w, color.h);
    set_rect(p, "DLSSNR.Output", ow, oh);
    if (have_guides) {
        fill(g.r_depth, g.depth, false);
        fill(g.r_mv, g.mv, false);
        p.Set("DLSSNR.Depth", static_cast<void*>(&g.r_depth));
        p.Set("DLSSNR.MVec", static_cast<void*>(&g.r_mv));
        set_rect(p, "DLSSNR.Depth", g.depth.w, g.depth.h);
        set_rect(p, "DLSSNR.MVec", g.mv.w, g.mv.h);
        float ms = static_cast<float>(env_f("GOWA_DLSSNR_MVSCALE", 1.0));
        p.Set("DLSSNR.MVecScaleX", ms);
        p.Set("DLSSNR.MVecScaleY", ms);
    }
    p.Set("DLSSNR.Width", ow);
    p.Set("DLSSNR.Height", oh);
    p.Set("DLSSNR.ScalingRatio", scaling_ratio());
    p.Set("DLSSNR.Reset", g.prev_ran ? 0 : 1);
    set_tuning(p);
    vkCmdResetQueryPool(cmd, g.queries, 0, 2);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g.queries, 0);
    unsigned int r = ngx(g.evaluate, cmd, g.handle, g.eval_params, nullptr);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.queries, 1);
    g.query_pending = true;
    g.out.layout = VK_IMAGE_LAYOUT_GENERAL;
    if (r != NVSDK_NGX_Result_Success) {
        if (g.fails++ < 5) LOG_ERROR("dlssnr", "EvaluateFeature failed: 0x%08X", r);
        if (layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) image_barrier(cmd, src, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout);
        g.prev_ran = false;
        return false;
    }
    g.evals++;
    if (g.total_evals++ == 0) LOG_INFO("dlssnr", "first evaluate ok: color %ux%u -> output %ux%u, guides %s", color.w, color.h, ow, oh, have_guides ? "depth+mv" : "none");
    g.prev_ran = true;
    g.src_w = w;
    g.src_h = h;
    if (mode() == 1) {
        to(cmd, g.out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        image_barrier(cmd, src, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        blit(cmd, g.out.image, ow, oh, src, w, h, VK_FILTER_NEAREST);
        image_barrier(cmd, src, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layout);
    } else {
        to(cmd, g.out, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) image_barrier(cmd, src, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout);
    }
    return true;
}

bool output(VkImage& image, VkImageLayout& layout, uint32_t& w, uint32_t& h) {
    if (!active() || !g.ran || !g.prev_ran || mode() != 2) return false;
    image = g.out.image;
    layout = g.out.layout;
    w = g.out.w;
    h = g.out.h;
    return true;
}

bool compose(VkCommandBuffer cmd, VkImageView final_view, VkImageLayout final_layout, uint32_t w, uint32_t h, VkImage& image, VkImageLayout*& layout, uint32_t& ow, uint32_t& oh) {
    if (!active() || mode() != 2 || !g.ran || !g.prev_ran || !init_compose()) return false;
    if (!ensure(g.comp, VK_FORMAT_R8G8B8A8_UNORM, g.out.w, g.out.h, VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    to(cmd, g.comp, VK_IMAGE_LAYOUT_GENERAL);
    to(cmd, g.out, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    to(cmd, g.hud, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkDescriptorImageInfo ii[4] = {
        {g.linear, final_view, final_layout},
        {g.linear, g.hud.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {g.linear, g.out.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, g.comp.view, VK_IMAGE_LAYOUT_GENERAL},
    };
    VkWriteDescriptorSet wr[4]{};
    for (uint32_t i = 0; i < 4; i++) wr[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, i, 0, 1, i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii[i], nullptr, nullptr};
    static const float thr = static_cast<float>(env_f("GOWA_DLSSNR_HUD_THRESHOLD", 3.0 / 255.0));
    float pc[8] = {float(g.src_w), float(g.src_h), float(w), float(h), float(g.comp.w), float(g.comp.h), thr, 0.0f};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.pipeline);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.pipe_layout, 0, 4, wr);
    vkCmdPushConstants(cmd, g.pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cmd, (g.comp.w + 7) / 8, (g.comp.h + 7) / 8, 1);
    to(cmd, g.comp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    image = g.comp.image;
    layout = &g.comp.layout;
    ow = g.comp.w;
    oh = g.comp.h;
    g.composed = true;
    return true;
}

bool snapshot_images(VkCommandBuffer cmd, VkImage& in, uint32_t& w, uint32_t& h, VkImage& out, uint32_t& ow, uint32_t& oh) {
    if (!active() || !g.ran || !g.prev_ran || !g.hud.image || !g.out.image) return false;
    to(cmd, g.hud, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    to(cmd, g.out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    in = g.hud.image;
    w = g.hud.w;
    h = g.hud.h;
    out = g.out.image;
    ow = g.out.w;
    oh = g.out.h;
    return true;
}

void end_frame() {
    if (!g.ok) return;
    g.frames++;
    if (!g.ran) g.prev_ran = false;
    auto now = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(now - g.stat_start).count();
    if (secs >= 5.0) {
        LOG_INFO("dlssnr", "%.1f fps, NR evaluated %llu of %llu frames, NR GPU %.2f ms avg, failures %llu", g.frames / secs, static_cast<unsigned long long>(g.evals), static_cast<unsigned long long>(g.frames),
                 g.gpu_n ? g.gpu_ms / g.gpu_n : 0.0, static_cast<unsigned long long>(g.fails));
        g.frames = g.evals = 0;
        g.gpu_ms = 0.0;
        g.gpu_n = 0;
        g.stat_start = now;
    }
    g.ran = false;
    g.inputs = false;
    g.composed = false;
}

void shutdown() {
    if (!g.ok) return;
    vkDeviceWaitIdle(g.device);
    if (g.handle) ngx(g.release, g.handle);
    g.handle = nullptr;
    if (g.shutdown1) ngx(g.shutdown1, g.device);
    g.ok = false;
}

}

#else

namespace gpu::dlssnr {

int mode() {
    const char* v = std::getenv("GOWA_DLSSNR");
    int r = v ? std::atoi(v) : 0;
    return r == 1 || r == 2 ? r : 0;
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
