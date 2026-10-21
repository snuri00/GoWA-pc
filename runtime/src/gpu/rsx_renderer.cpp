#include "runtime/placement.h"
#include <immintrin.h>
#include "gpu/presenter.h"
#include "motion_vectors.h"
#include "dlssg.h"
#include "gpu/rsx.h"
#include "runtime/log.h"
#include "runtime/trace.h"
#include "runtime/memory.h"
#include "fp_decompiler.h"
#include "ring.h"
#include "shader_compiler.h"
#include "texture_decode.h"
#include "vk_context.h"
#include "vp_decompiler.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <map>
#include <set>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace lv2 {
uint32_t rsx_address(uint32_t offset, uint32_t loc);
}

namespace gpu::rsx {

namespace {

enum : uint32_t {
    SURFACE_CLIP_HORIZONTAL = 0x0200,
    SURFACE_CLIP_VERTICAL = 0x0204,
    SURFACE_FORMAT = 0x0208,
    SURFACE_COLOR_AOFFSET = 0x0210,
    SURFACE_ZETA_OFFSET = 0x0214,
    SURFACE_COLOR_BOFFSET = 0x0218,
    SURFACE_COLOR_TARGET = 0x0220,
    SURFACE_COLOR_COFFSET = 0x0288,
    SURFACE_COLOR_DOFFSET = 0x028C,
    CONTEXT_DMA_COLOR_A = 0x0194,
    CONTEXT_DMA_COLOR_B = 0x018C,
    CONTEXT_DMA_ZETA = 0x0198,
    ALPHA_TEST_ENABLE = 0x0304,
    ALPHA_FUNC = 0x0308,
    ALPHA_REF = 0x030C,
    BLEND_ENABLE = 0x0310,
    BLEND_SFACTOR = 0x0314,
    BLEND_DFACTOR = 0x0318,
    BLEND_COLOR = 0x031C,
    BLEND_EQUATION = 0x0320,
    COLOR_MASK = 0x0324,
    STENCIL_TEST_ENABLE = 0x0328,
    STENCIL_MASK = 0x032C,
    STENCIL_FUNC = 0x0330,
    STENCIL_REF = 0x0334,
    STENCIL_FUNC_MASK = 0x0338,
    STENCIL_OP_FAIL = 0x033C,
    TWO_SIDED_STENCIL = 0x0348,
    BACK_STENCIL_MASK = 0x034C,
    BACK_STENCIL_FUNC = 0x0350,
    BACK_STENCIL_REF = 0x0354,
    BACK_STENCIL_FUNC_MASK = 0x0358,
    BACK_STENCIL_OP_FAIL = 0x035C,
    BLEND_ENABLE_MRT = 0x036C,
    COLOR_MASK_MRT = 0x0370,
    LOGIC_OP_ENABLE = 0x0374,
    LOGIC_OP = 0x0378,
    DEPTH_BOUNDS_ENABLE = 0x0380,
    DEPTH_BOUNDS_MIN = 0x0384,
    DEPTH_BOUNDS_MAX = 0x0388,
    LINE_WIDTH = 0x03B8,
    SCISSOR_HORIZONTAL = 0x08C0,
    SCISSOR_VERTICAL = 0x08C4,
    FOG_MODE = 0x08CC,
    FOG_PARAMS = 0x08D0,
    SHADER_PROGRAM = 0x08E4,
    VIEWPORT_OFFSET = 0x0A20,
    VIEWPORT_SCALE = 0x0A30,
    POLY_OFFSET_FILL = 0x0A68,
    DEPTH_FUNC = 0x0A6C,
    DEPTH_MASK = 0x0A70,
    DEPTH_TEST_ENABLE = 0x0A74,
    POLY_OFFSET_SCALE = 0x0A78,
    POLY_OFFSET_BIAS = 0x0A7C,
    TRANSFORM_PROGRAM = 0x0B80,
    TWO_SIDE_LIGHT = 0x142C,
    VERTEX_ARRAY_OFFSET = 0x1680,
    VERTEX_BASE_OFFSET = 0x1738,
    VERTEX_BASE_INDEX = 0x173C,
    VERTEX_ARRAY_FORMAT = 0x1740,
    BEGIN_END = 0x1808,
    ARRAY_ELEMENT16 = 0x180C,
    ARRAY_ELEMENT32 = 0x1810,
    DRAW_ARRAYS = 0x1814,
    INLINE_ARRAY = 0x1818,
    INDEX_ARRAY_ADDRESS = 0x181C,
    INDEX_ARRAY_DMA = 0x1820,
    DRAW_INDEX_ARRAY = 0x1824,
    FRONT_POLYGON_MODE = 0x1828,
    CULL_FACE = 0x1830,
    FRONT_FACE = 0x1834,
    CULL_FACE_ENABLE = 0x183C,
    TEXTURE_CONTROL3 = 0x1840,
    TEXTURE_BASE = 0x1A00,
    SHADER_CONTROL = 0x1D60,
    SHADER_WINDOW = 0x1D88,
    ZSTENCIL_CLEAR_VALUE = 0x1D8C,
    COLOR_CLEAR_VALUE = 0x1D90,
    CLEAR_SURFACE = 0x1D94,
    RESTART_INDEX_ENABLE = 0x1DAC,
    RESTART_INDEX = 0x1DB0,
    TRANSFORM_PROGRAM_LOAD = 0x1E9C,
    TRANSFORM_PROGRAM_START = 0x1EA0,
    POINT_SIZE = 0x1EE0,
    TRANSFORM_CONSTANT_LOAD = 0x1EFC,
    TRANSFORM_CONSTANT = 0x1F00,
    FREQUENCY_DIVIDER = 0x1FC0,
    SHADER_PACKER = 0x1FEC,
    OUTPUT_MASK = 0x1FF4,
    BRANCH_BITS = 0x1FF8,
};

constexpr uint32_t CELL_GCM_LOCATION_LOCAL = 0xFEED0000;
constexpr uint32_t RESTART = 0xFFFFFFFFu;

struct Surface {
    uint32_t offset = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::map<uint64_t, VkImageView> sample_views;
    uint64_t generation = 0;
    bool main = false;
    uint32_t pitch = 0;
    bool dirty = false;
    float scale = 1.0f;
    uint32_t iw = 0;
    uint32_t ih = 0;
    bool depth() const { return aspect != VK_IMAGE_ASPECT_COLOR_BIT; }
};

float res_scale() {
    static const float s = [] {
        const char* e = std::getenv("GOWA_RES_SCALE");
        float v = e ? static_cast<float>(std::atof(e)) : 1.0f;
        return v >= 1.0f && v <= 4.0f ? v : 1.0f;
    }();
    return s;
}

uint32_t up(uint32_t v, float s) { return std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(double(v) * s))); }

struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    TextureLayout layout;
    uint64_t hash = 0;
    uint64_t checked = ~0ull;
    uint64_t last_used = 0;
    uint64_t epoch = 0;
    bool watched = false;
    uint32_t churn = 0;
    uint64_t nowatch_until = 0;
    VkImageLayout img_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t> pool_key;
};

struct PooledImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

VkDeviceSize g_pool_bytes = 0;

struct VpEntry {
    std::atomic<bool> ready{false};
    bool ok = false;
    uint64_t hash = 0;
    VkShaderModule module = VK_NULL_HANDLE;
    uint32_t inputs = 0;
};

struct FpEntry {
    std::atomic<bool> ready{false};
    bool ok = false;
    uint64_t hash = 0;
    VkShaderModule module = VK_NULL_HANDLE;
    std::vector<uint32_t> literal_slots;
    uint32_t start_slot = 0;
    uint32_t textures = 0;
    uint32_t dims = 0;
};

struct Pass {
    bool active = false;
    Surface* colors[4] = {};
    uint32_t color_count = 0;
    Surface* depth = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct Dummy {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

uint32_t g_regs[0x4000];
std::map<std::tuple<uint32_t, VkFormat, uint32_t>, std::unique_ptr<Surface>> g_surfaces;
uint64_t g_surfaces_version = 0;
uint64_t g_surface_index_version = ~0ull;
std::unordered_map<uint32_t, std::vector<Surface*>> g_surface_index;

const std::vector<Surface*>& surfaces_at(uint32_t offset) {
    static const std::vector<Surface*> none;
    if (g_surface_index_version != g_surfaces_version) {
        g_surface_index.clear();
        for (auto& [k, sp] : g_surfaces) g_surface_index[sp->offset].push_back(sp.get());
        g_surface_index_version = g_surfaces_version;
    }
    auto it = g_surface_index.find(offset);
    return it == g_surface_index.end() ? none : it->second;
}
std::vector<std::pair<uint64_t, std::unique_ptr<Surface>>> g_retired_surfaces;

struct TempImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    float scale = 1.0f;
    uint32_t iw = 0;
    uint32_t ih = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::map<uint64_t, VkImageView> views;
    uint64_t generation = ~0ull;
    uint64_t last_used = 0;
};

std::map<std::tuple<Surface*, uint32_t, uint32_t, bool>, std::unique_ptr<TempImage>> g_temps;
struct TexKeyHash {
    size_t operator()(const std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>& k) const {
        uint64_t h = (uint64_t(std::get<0>(k)) << 32) ^ std::get<1>(k);
        h = (h ^ (h >> 29)) * 0xBF58476D1CE4E5B9ull;
        h ^= (uint64_t(std::get<2>(k)) << 32) ^ std::get<3>(k);
        h = (h ^ (h >> 31)) * 0x94D049BB133111EBull;
        h ^= (uint64_t(std::get<4>(k)) << 32) ^ std::get<5>(k);
        return size_t(h ^ (h >> 33));
    }
};
std::unordered_map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>, std::unique_ptr<Texture>, TexKeyHash> g_textures;
std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>, std::vector<PooledImage>> g_image_pool;
std::vector<std::pair<uint64_t, std::unique_ptr<Texture>>> g_retired_textures;
std::unordered_map<uint64_t, VkSampler> g_samplers;
struct PipelineEntry {
    std::atomic<bool> ready{false};
    VkPipeline pipeline = VK_NULL_HANDLE;
};

std::unordered_map<uint64_t, PipelineEntry> g_pipelines;
std::unordered_map<uint64_t, VpEntry> g_vp_cache;
std::unordered_map<uint64_t, FpEntry> g_fp_cache;
uint32_t g_vp_ucode[544 * 4];
uint32_t g_vp_load_word = 0;
float g_vp_constants[468 * 4];
bool g_vp_constants_dirty = true;
RingAlloc g_vp_constants_alloc;
uint64_t g_vp_constants_serial = ~0ull;
float g_imm[16][4];
std::atomic<uint32_t> g_vp_failures{0};
std::atomic<uint32_t> g_fp_failures{0};
uint64_t g_clears = 0;
Pass g_pass;
VkQueryPool g_qpool = VK_NULL_HANDLE;
constexpr uint32_t QMAX = 32768;
uint32_t g_qused = 0;
bool g_qreset_needed = true;
std::vector<uint32_t> g_counter_queries;
struct PendingReport {
    uint32_t ea;
    std::vector<uint32_t> queries;
};
std::vector<PendingReport> g_pending_reports;
std::unordered_map<uint32_t, uint32_t> g_zpass_cache;
Dummy g_dummy[4];
VkDescriptorSetLayout g_set_layout = VK_NULL_HANDLE;
VkPipelineLayout g_pipeline_layout = VK_NULL_HANDLE;
VkPipelineCache g_pipeline_cache = VK_NULL_HANDLE;
uint64_t g_pipelines_saved = 0;

void save_pipeline_cache() {
    if (!g_pipeline_cache) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(vk().device, g_pipeline_cache, &size, nullptr) != VK_SUCCESS || !size) return;
    std::vector<char> blob(size);
    if (vkGetPipelineCacheData(vk().device, g_pipeline_cache, &size, blob.data()) != VK_SUCCESS) return;
    std::error_code ec;
    std::filesystem::create_directories("shader_cache", ec);
    FILE* f = std::fopen("shader_cache/pipelines.bin.tmp", "wb");
    if (!f) return;
    bool ok = std::fwrite(blob.data(), 1, size, f) == size;
    std::fclose(f);
    if (ok) std::filesystem::rename("shader_cache/pipelines.bin.tmp", "shader_cache/pipelines.bin", ec);
}
bool g_ready = false;
bool g_trace = false;
int g_trace_serial = 0;
uint32_t g_frame_draw = 0;
struct VbCheck {
    uint32_t draw, attr, ea, size;
    uint64_t hash;
};
std::vector<VbCheck> g_vb_checks;
uint64_t vb_hash(uint32_t ea, uint32_t size) {
    uint64_t h = 1469598103934665603ull;
    const uint8_t* p = vm::ptr(ea);
    for (uint32_t i = 0; i < size; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
uint32_t g_skip_lo = 0, g_skip_hi = 0;
bool g_skip_init = [] {
    if (const char* e = std::getenv("GOWA_SKIP_DRAWS")) {
        g_skip_lo = static_cast<uint32_t>(std::strtoul(e, nullptr, 0));
        const char* d = std::strchr(e, '-');
        g_skip_hi = d ? static_cast<uint32_t>(std::strtoul(d + 1, nullptr, 0)) : g_skip_lo;
        return true;
    }
    return false;
}();
uint32_t g_trace_draws = 0;
void schedule_dumps();
uint32_t g_experiment = std::getenv("GOWA_GPU_EXPERIMENT") ? std::atoi(std::getenv("GOWA_GPU_EXPERIMENT")) : 0;

uint32_t g_prim = 0;
bool g_in_begin = false;
std::vector<std::pair<uint32_t, uint32_t>> g_ranges;
bool g_indexed = false;
std::vector<uint32_t> g_inline;
std::vector<uint32_t> g_elements;

struct CompileFrame {
    std::chrono::steady_clock::duration time{};
    uint32_t vp = 0;
    uint32_t fp = 0;
    uint32_t pipelines = 0;
};

CompileFrame g_compile_frame;

struct CompileTimer {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~CompileTimer() { g_compile_frame.time += std::chrono::steady_clock::now() - t0; }
};

struct Stats {
    uint64_t draws = 0;
    uint64_t skipped = 0;
    uint64_t vertices = 0;
    uint64_t pipelines = 0;
    uint64_t uploads = 0;
    uint64_t draw_ticks = 0;
    uint64_t tex_ticks = 0;
    uint64_t present_ticks = 0;
    uint64_t phase[8] = {};
    uint64_t copies = 0;
    uint64_t copy_bytes = 0;
    uint64_t redirects = 0;
    uint64_t hash_bytes = 0;
    uint64_t hash_count = 0;
    uint64_t hash_ticks = 0;
    uint64_t upload_frame = 0;
    uint64_t upload_bytes = 0;
    uint64_t pool_hits = 0;
    uint64_t image_allocs = 0;
    uint64_t textures_freed = 0;
    uint64_t watch_skips = 0;
    uint64_t blit_fast = 0;
    uint64_t blit_slow = 0;
} g_stats;

uint64_t ticks() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return static_cast<uint64_t>(v.QuadPart);
}

uint32_t R(uint32_t method) { return g_regs[(method >> 2) & 0x3FFF]; }
float RF(uint32_t method) {
    float f;
    uint32_t v = R(method);
    std::memcpy(&f, &v, 4);
    return f;
}

uint64_t fnv(const void* data, size_t size, uint64_t h = 1469598103934665603ull) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

uint64_t fast_hash(const uint8_t* p, size_t size) {
    const __m256i k = _mm256_set1_epi64x(0x9E3779B1ll);
    __m256i a = _mm256_set1_epi64x(static_cast<long long>(0x9E3779B97F4A7C15ull ^ size));
    __m256i b = _mm256_set1_epi64x(static_cast<long long>(0xC2B2AE3D27D4EB4Full));
    size_t i = 0;
    for (; i + 64 <= size; i += 64) {
        a = _mm256_xor_si256(a, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + i)));
        b = _mm256_xor_si256(b, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + i + 32)));
        a = _mm256_add_epi64(_mm256_mul_epu32(a, k), _mm256_srli_epi64(a, 29));
        b = _mm256_add_epi64(_mm256_mul_epu32(b, k), _mm256_srli_epi64(b, 29));
    }
    alignas(32) uint64_t la[4], lb[4];
    _mm256_store_si256(reinterpret_cast<__m256i*>(la), a);
    _mm256_store_si256(reinterpret_cast<__m256i*>(lb), b);
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (int l = 0; l < 4; l++) {
        h = (h ^ la[l]) * 0xFF51AFD7ED558CCDull;
        h = (h ^ lb[l]) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 29;
    }
    for (; i < size; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

bool readable(uint32_t ea, uint64_t size) { return vm::range_mapped(ea, size); }

float half_to_float(uint16_t h) { return _cvtsh_ss(h); }

VkShaderModule make_module(const std::vector<uint32_t>& spirv) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spirv.size() * 4;
    ci.pCode = spirv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(vk().device, &ci, nullptr, &m);
    return m;
}

void dump_failure(const char* prefix, uint64_t h, const std::string& glsl, const std::string& log) {
    char fn[64];
    std::snprintf(fn, sizeof fn, "%s_%016llx.glsl", prefix, static_cast<unsigned long long>(h));
    if (FILE* f = std::fopen(fn, "w")) {
        std::fputs(glsl.c_str(), f);
        std::fputs("\n/*\n", f);
        std::fputs(log.c_str(), f);
        std::fputs("\n*/\n", f);
        std::fclose(f);
    }
}

class CompilePool {
public:
    void submit(std::function<void()> job) {
        std::call_once(started_, [this] {
            for (int i = 0; i < 2; i++) {
                std::thread([this] { run(); }).detach();
            }
        });
        {
            std::lock_guard<std::mutex> lock(m_);
            q_.push_back(std::move(job));
        }
        cv_.notify_one();
    }

private:
    void run() {
        SetThreadDescription(GetCurrentThread(), L"Shader compiler");
        placement::apply("shader");
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(m_);
                cv_.wait(lock, [this] { return !q_.empty(); });
                job = std::move(q_.front());
                q_.pop_front();
            }
            job();
        }
    }

    std::once_flag started_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
};

CompilePool g_compile_pool;

bool async_compile() {
    static const bool on = [] {
        const char* e = std::getenv("GOWA_ASYNC_SHADERS");
        return !e || e[0] != '0';
    }();
    return on;
}

void finish_vp(VpEntry& e, const std::string& glsl) {
    std::vector<uint32_t> spirv;
    std::string log;
    e.ok = compile_glsl(ShaderStage::Vertex, glsl, spirv, log);
    if (e.ok) e.module = make_module(spirv);
    else if (g_vp_failures++ < 20) dump_failure("vp_fail", e.hash, glsl, log);
    e.ready.store(true, std::memory_order_release);
}

void finish_fp(FpEntry& e, const std::string& glsl) {
    std::vector<uint32_t> spirv;
    std::string log;
    e.ok = compile_glsl(ShaderStage::Fragment, glsl, spirv, log);
    if (e.ok) e.module = make_module(spirv);
    else if (g_fp_failures++ < 20) dump_failure("fp_fail", e.hash, glsl, log);
    e.ready.store(true, std::memory_order_release);
}

template <typename Entry>
void compile_entry(Entry& e, ShaderStage stage, std::string glsl, void (*finish)(Entry&, const std::string&)) {
    std::vector<uint32_t> spirv;
    if (!async_compile() || load_cached_glsl(stage, glsl, spirv)) {
        finish(e, glsl);
        return;
    }
    g_compile_pool.submit([&e, glsl = std::move(glsl), finish] { finish(e, glsl); });
}

uint64_t g_vp_ucode_serial = 0;

uint32_t g_vp_canon[544 * 4];
uint32_t g_vp_canon_start = 0;

bool vp_branch(uint32_t sca) { return sca == 0x09 || sca == 0x0A || sca == 0x11 || sca == 0x12; }

uint32_t vp_target(const uint32_t* w) { return (((w[0] >> 23) & 1) << 9) | ((w[2] & 0x3F) << 3) | ((w[3] >> 29) & 7); }

uint64_t hash_vertex_program(uint32_t start) {
    std::bitset<544> seen;
    uint32_t work[544];
    uint32_t nwork = 0;
    work[nwork++] = start;
    bool relocatable = true;
    while (nwork) {
        uint32_t s = work[--nwork];
        if (s < start) relocatable = false;
        while (s < 544 && !seen[s]) {
            seen[s] = true;
            const uint32_t* w = &g_vp_ucode[s * 4];
            if (vp_branch((w[1] >> 27) & 0x1F) && nwork < 544) work[nwork++] = vp_target(w);
            if (w[3] & 1) break;
            s++;
        }
    }
    uint32_t base = relocatable ? start : 0;
    std::memset(g_vp_canon, 0, sizeof g_vp_canon);
    for (uint32_t s = 0; s < 544; s++) {
        if (!seen[s]) continue;
        uint32_t* d = &g_vp_canon[(s - base) * 4];
        std::memcpy(d, &g_vp_ucode[s * 4], 16);
        if (base && vp_branch((d[1] >> 27) & 0x1F)) {
            uint32_t t = vp_target(d) - base;
            d[0] = (d[0] & ~(1u << 23)) | (((t >> 9) & 1) << 23);
            d[2] = (d[2] & ~0x3Fu) | ((t >> 3) & 0x3F);
            d[3] = (d[3] & ~(7u << 29)) | ((t & 7) << 29);
        }
    }
    g_vp_canon_start = start - base;
    uint64_t h = 1469598103934665603ull ^ g_vp_canon_start;
    for (uint32_t s = 0; s < 544; s++) {
        if (!seen[s]) continue;
        uint32_t r = s - base;
        h = fnv(&r, 4, h);
        h = fnv(&g_vp_canon[r * 4], 16, h);
    }
    return h;
}

VpEntry* vertex_program() {
    uint32_t start = R(TRANSFORM_PROGRAM_START);
    static std::unordered_map<uint32_t, std::pair<uint64_t, VpEntry*>> recent;
    auto rit = recent.find(start);
    if (rit != recent.end() && rit->second.first == g_vp_ucode_serial) return rit->second.second;
    uint64_t h = hash_vertex_program(start);
    auto it = g_vp_cache.find(h);
    if (it != g_vp_cache.end()) {
        recent[start] = {g_vp_ucode_serial, &it->second};
        return &it->second;
    }
    CompileTimer ct;
    g_compile_frame.vp++;
    VpEntry& e = g_vp_cache[h];
    e.hash = h;
    VertexProgramInfo vp = decompile_vertex_program(g_vp_canon, g_vp_canon_start);
    e.inputs = vp.input_mask | 1u;
    if (vp.valid) {
        compile_entry(e, ShaderStage::Vertex, vp.glsl, finish_vp);
    } else {
        if (g_vp_failures++ < 20) dump_failure("vp_fail", h, vp.glsl, "");
        e.ready.store(true, std::memory_order_release);
    }
    if (std::getenv("GOWA_SHADER_DUMP")) {
        std::string words;
        for (uint32_t k = start; k < 544; k++) {
            char b[64];
            std::snprintf(b, sizeof b, "%u: %08x %08x %08x %08x\n", k, g_vp_ucode[k * 4], g_vp_ucode[k * 4 + 1], g_vp_ucode[k * 4 + 2], g_vp_ucode[k * 4 + 3]);
            words += b;
            if (g_vp_ucode[k * 4 + 3] & 1) break;
        }
        dump_failure("vp", h, vp.glsl, words);
    }
    LOG_INFO("rsx", "vertex program %016llx start %u: %s (%zu seen, %u failures)", static_cast<unsigned long long>(h), start, !e.ready ? "queued" : e.ok ? "ok" : "FAILED", g_vp_cache.size(), g_vp_failures.load());
    return &e;
}

uint32_t texture_dims() {
    uint32_t d = 0;
    for (uint32_t u = 0; u < 16; u++) {
        uint32_t f = R(TEXTURE_BASE + 4 + u * 0x20);
        uint32_t dim = (f >> 4) & 0xF;
        uint32_t v = dim == 1 ? 0 : dim == 3 ? 3 : (f & 4) ? 2 : 1;
        d |= v << (u * 2);
    }
    return d;
}

uint32_t color_mask_bits() {
    switch (R(SURFACE_COLOR_TARGET)) {
    case 0x01: return 1;
    case 0x02: return 2;
    case 0x13: return 3;
    case 0x17: return 7;
    case 0x1F: return 15;
    default: return 0;
    }
}

uint32_t mrt_count() {
    uint32_t m = color_mask_bits();
    uint32_t n = 0;
    for (uint32_t i = 0; i < 4; i++) n += (m >> i) & 1;
    return n ? n : 1;
}

uint32_t fp_address() {
    uint32_t prog = R(SHADER_PROGRAM);
    return lv2::rsx_address(prog & ~3u, (prog & 3) - 1);
}

struct FpFast {
    uint64_t program_hash = 0;
    uint64_t h = 0;
    uint32_t texture_mask = 0;
    uint32_t slots = 0;
    std::vector<uint8_t> skip;
    std::vector<uint8_t> bytes;
    FpEntry* entry = nullptr;
};

std::unordered_map<uint32_t, FpFast> g_fp_fast;

bool fp_fast_valid(const FpFast& f, uint32_t ea) {
    if (!readable(ea, uint64_t(f.slots) * 16)) return false;
    const uint8_t* p = vm::ptr(ea);
    for (uint32_t i = 0; i < f.slots; i++) {
        if (f.skip[i]) continue;
        if (std::memcmp(p + i * 16, f.bytes.data() + i * 16, 16) != 0) return false;
    }
    return true;
}

FragmentProgramKey fp_key(uint32_t texture_mask) {
    FragmentProgramKey key;
    key.control = R(SHADER_CONTROL) & 0xCE;
    uint32_t dims = texture_dims();
    for (uint32_t u = 0; u < 16; u++) {
        if (!(texture_mask & (1u << u))) continue;
        key.texture_dims |= dims & (3u << (u * 2));
        uint32_t tfmt = R(TEXTURE_BASE + 4 + u * 0x20);
        if (tfmt & 0x4000) key.unnormalized |= 1u << u;
        uint32_t tcode = ((tfmt >> 8) & 0xFF) & ~0x60u;
        uint32_t tzf = (R(TEXTURE_BASE + 8 + u * 0x20) >> 28) & 7;
        if (tcode >= 0x90 && tcode <= 0x93 && tzf != 0 && tzf != 7) key.shadow |= 1u << u;
    }
    key.mrt_count = mrt_count();
    return key;
}

uint64_t fp_key_hash(uint64_t program_hash, const FragmentProgramKey& key) {
    return program_hash ^ (uint64_t(key.control) << 1) ^ (uint64_t(key.texture_dims) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(key.mrt_count) << 40) ^ (uint64_t(key.unnormalized) * 0xC2B2AE3D27D4EB4Full) ^
           (uint64_t(key.shadow) << 48);
}

FpEntry* fragment_program(uint32_t ea) {
    if (!ea) return nullptr;
    auto fit = g_fp_fast.find(ea);
    if (fit != g_fp_fast.end() && fp_fast_valid(fit->second, ea)) {
        uint64_t fh = fp_key_hash(fit->second.program_hash, fp_key(fit->second.texture_mask));
        if (fh == fit->second.h) return fit->second.entry;
        auto cit = g_fp_cache.find(fh);
        if (cit != g_fp_cache.end()) {
            fit->second.h = fh;
            fit->second.entry = &cit->second;
            return &cit->second;
        }
    }
    FpWordReader read = [ea](uint32_t i) {
        uint32_t v = vm::read32(ea + i * 4);
        return (v << 16) | (v >> 16);
    };
    FragmentProgramInfo a = analyse_fragment_program(read);
    if (!a.valid) return nullptr;
    FragmentProgramKey key = fp_key(a.texture_mask);
    uint64_t h = fp_key_hash(a.hash, key);
    auto remember = [&](FpEntry* e) {
        uint32_t slots = a.start_slot + a.length_slots;
        if (!slots || slots > 4096 || !readable(ea, uint64_t(slots) * 16)) return;
        FpFast& f = g_fp_fast[ea];
        f.program_hash = a.hash;
        f.h = h;
        f.texture_mask = a.texture_mask;
        f.slots = slots;
        f.skip.assign(slots, 0);
        for (uint32_t ls : a.literal_slots) {
            if (a.start_slot + ls < slots) f.skip[a.start_slot + ls] = 1;
        }
        f.bytes.assign(vm::ptr(ea), vm::ptr(ea) + size_t(slots) * 16);
        f.entry = e;
    };
    auto it = g_fp_cache.find(h);
    if (it != g_fp_cache.end()) {
        remember(&it->second);
        return &it->second;
    }
    CompileTimer ct;
    g_compile_frame.fp++;
    FpEntry& e = g_fp_cache[h];
    e.hash = h;
    FragmentProgramInfo fp = decompile_fragment_program(read, key);
    e.literal_slots = fp.literal_slots;
    e.start_slot = fp.start_slot;
    e.textures = fp.texture_mask;
    e.dims = key.texture_dims;
    if (std::getenv("GOWA_SHADER_DUMP")) dump_failure("fp", h, fp.glsl, "");
    if (fp.valid) {
        compile_entry(e, ShaderStage::Fragment, fp.glsl, finish_fp);
    } else {
        if (g_fp_failures++ < 20) dump_failure("fp_fail", h, fp.glsl, "");
        e.ready.store(true, std::memory_order_release);
    }
    LOG_INFO("rsx", "fragment program %016llx at 0x%08x (%u slots, %zu literals): %s (%zu seen, %u failures)", static_cast<unsigned long long>(h), ea, fp.length_slots,
             fp.literal_slots.size(), !e.ready ? "queued" : e.ok ? "ok" : "FAILED", g_fp_cache.size(), g_fp_failures.load());
    return &e;
}

VkFormat color_format(uint32_t fmt) {
    switch (fmt & 0x1F) {
    case 0x1: case 0x2: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case 0x3: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case 0x9: return VK_FORMAT_R8_UNORM;
    case 0xA: return VK_FORMAT_R8G8_UNORM;
    case 0xB: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0xC: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0xD: return VK_FORMAT_R32_SFLOAT;
    case 0xE: case 0xF: case 0x10: return VK_FORMAT_R8G8B8A8_UNORM;
    default: return VK_FORMAT_B8G8R8A8_UNORM;
    }
}

VkFormat depth_format(uint32_t fmt) { return ((fmt >> 5) & 7) == 1 ? VK_FORMAT_D16_UNORM : VK_FORMAT_D24_UNORM_S8_UINT; }

VkImageView make_view(VkImage image, VkFormat format, VkImageViewType type, VkImageAspectFlags aspect, VkComponentMapping mapping, uint32_t levels, uint32_t layers) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = type;
    vi.format = format;
    vi.components = mapping;
    vi.subresourceRange = {aspect, 0, levels, 0, layers};
    VkImageView v = VK_NULL_HANDLE;
    vkCreateImageView(vk().device, &vi, nullptr, &v);
    return v;
}

struct MemBlock {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t type = 0;
    VkDeviceSize size = 0;
    std::map<VkDeviceSize, VkDeviceSize> free;
};

struct SubAlloc {
    MemBlock* block;
    VkDeviceSize offset;
    VkDeviceSize size;
};

std::vector<std::unique_ptr<MemBlock>> g_mem_blocks;
std::unordered_map<VkImage, SubAlloc> g_suballocs;
constexpr VkDeviceSize kBlockSize = VkDeviceSize(128) << 20;

bool suballocate(MemBlock& b, VkDeviceSize size, VkDeviceSize align, VkDeviceSize& out) {
    for (auto it = b.free.begin(); it != b.free.end(); ++it) {
        VkDeviceSize o = (it->first + align - 1) & ~(align - 1);
        VkDeviceSize end = it->first + it->second;
        if (o + size > end) continue;
        VkDeviceSize start = it->first;
        b.free.erase(it);
        if (o > start) b.free[start] = o - start;
        if (o + size < end) b.free[o + size] = end - (o + size);
        out = o;
        return true;
    }
    return false;
}

void release_range(MemBlock& b, VkDeviceSize offset, VkDeviceSize size) {
    auto it = b.free.emplace(offset, size).first;
    auto next = std::next(it);
    if (next != b.free.end() && it->first + it->second == next->first) {
        it->second += next->second;
        b.free.erase(next);
    }
    if (it != b.free.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second == it->first) {
            prev->second += it->second;
            b.free.erase(it);
        }
    }
}

bool allocate_image(const VkImageCreateInfo& ii, VkImage& image, VkDeviceMemory& memory) {
    const VkCtx& c = vk();
    if (vkCreateImage(c.device, &ii, nullptr, &image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(c.device, image, &req);
    uint32_t type = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (req.size <= kBlockSize / 4) {
        VkDeviceSize align = std::max<VkDeviceSize>(req.alignment, 256);
        for (auto& b : g_mem_blocks) {
            VkDeviceSize off;
            if (b->type != type || !suballocate(*b, req.size, align, off)) continue;
            vkBindImageMemory(c.device, image, b->memory, off);
            memory = b->memory;
            g_suballocs[image] = {b.get(), off, req.size};
            return true;
        }
        auto b = std::make_unique<MemBlock>();
        VkMemoryAllocateInfo bi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        bi.allocationSize = kBlockSize;
        bi.memoryTypeIndex = type;
        if (vkAllocateMemory(c.device, &bi, nullptr, &b->memory) == VK_SUCCESS) {
            b->type = type;
            b->size = kBlockSize;
            b->free[0] = kBlockSize;
            VkDeviceSize off = 0;
            suballocate(*b, req.size, align, off);
            vkBindImageMemory(c.device, image, b->memory, off);
            memory = b->memory;
            g_suballocs[image] = {b.get(), off, req.size};
            g_mem_blocks.push_back(std::move(b));
            return true;
        }
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(c.device, &ai, nullptr, &memory) != VK_SUCCESS) return false;
    vkBindImageMemory(c.device, image, memory, 0);
    return true;
}

void free_image(VkImage image, VkDeviceMemory memory) {
    const VkCtx& c = vk();
    vkDestroyImage(c.device, image, nullptr);
    auto it = g_suballocs.find(image);
    if (it != g_suballocs.end()) {
        release_range(*it->second.block, it->second.offset, it->second.size);
        g_suballocs.erase(it);
        return;
    }
    vkFreeMemory(c.device, memory, nullptr);
}

void end_pass();
void transition(Surface* s, VkImageLayout to);

VkFormat srgb_variant(VkFormat f) {
    if (f == VK_FORMAT_B8G8R8A8_UNORM) return VK_FORMAT_B8G8R8A8_SRGB;
    if (f == VK_FORMAT_R8G8B8A8_UNORM) return VK_FORMAT_R8G8B8A8_SRGB;
    return f;
}

Surface* surface(uint32_t offset, VkFormat format, uint32_t w, uint32_t h, bool depth, bool main = false, uint32_t pitch = 0) {
    auto key = std::make_tuple(main ? (offset | 0x80000000u) : offset, format, pitch);
    auto it = g_surfaces.find(key);
    Surface* old = it != g_surfaces.end() ? it->second.get() : nullptr;
    if (old && old->width >= w && old->height >= h) return old;
    uint32_t nw = old ? std::max(old->width, w) : w, nh = old ? std::max(old->height, h) : h;
    auto s = std::make_unique<Surface>();
    s->offset = offset;
    s->main = main;
    s->pitch = pitch;
    s->width = nw;
    s->height = nh;
    s->scale = main ? 1.0f : res_scale();
    s->iw = up(nw, s->scale);
    s->ih = up(nh, s->scale);
    s->format = format;
    s->aspect = depth ? (format == VK_FORMAT_D16_UNORM ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {s->iw, s->ih, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (!depth && srgb_variant(format) != format) ii.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (!allocate_image(ii, s->image, s->memory)) return nullptr;
    s->view = make_view(s->image, format, VK_IMAGE_VIEW_TYPE_2D, s->aspect, {}, 1, 1);
    LOG_INFO("rsx", "new %s surface at %s 0x%x %ux%u format %d pitch 0x%x%s", depth ? "depth" : "color", main ? "main" : "local", offset, nw, nh, int(format), pitch, old ? " (grown)" : "");
    Surface* raw = s.get();
    if (old) {
        end_pass();
        transition(old, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transition(raw, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy c{};
        c.srcSubresource = {old->aspect, 0, 0, 1};
        c.dstSubresource = {raw->aspect, 0, 0, 1};
        c.extent = {std::min(old->iw, raw->iw), std::min(old->ih, raw->ih), 1};
        vkCmdCopyImage(frame_cmd(), old->image, old->layout, raw->image, raw->layout, 1, &c);
        raw->generation = old->generation + 1;
        raw->dirty = old->dirty;
        for (auto t = g_temps.begin(); t != g_temps.end();) {
            if (std::get<0>(t->first) == old) t = g_temps.erase(t);
            else ++t;
        }
        g_retired_surfaces.emplace_back(frame_serial(), std::move(it->second));
        g_surfaces.erase(it);
        g_surfaces_version++;
    }
    g_surfaces[key] = std::move(s);
    g_surfaces_version++;
    return raw;
}

void destroy_surface(Surface* s) {
    const VkCtx& c = vk();
    for (auto& [k, v] : s->sample_views) vkDestroyImageView(c.device, v, nullptr);
    vkDestroyImageView(c.device, s->view, nullptr);
    free_image(s->image, s->memory);
}

void destroy_temp(TempImage* t) {
    const VkCtx& c = vk();
    for (auto& [k, v] : t->views) vkDestroyImageView(c.device, v, nullptr);
    free_image(t->image, t->memory);
}

void collect_garbage() {
    uint64_t serial = frame_serial();
    const VkCtx& c = vk();
    for (auto it = g_textures.begin(); it != g_textures.end();) {
        if (serial > it->second->last_used + 240) {
            vkDestroyImageView(c.device, it->second->view, nullptr);
            it->second->view = VK_NULL_HANDLE;
            g_retired_textures.emplace_back(serial, std::move(it->second));
            it = g_textures.erase(it);
            g_stats.textures_freed++;
        } else {
            ++it;
        }
    }
    for (auto it = g_retired_textures.begin(); it != g_retired_textures.end();) {
        if (serial > it->first + 4) {
            std::vector<PooledImage>& pool = g_image_pool[it->second->pool_key];
            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(c.device, it->second->image, &req);
            if (g_pool_bytes + req.size <= (VkDeviceSize(512) << 20)) {
                pool.push_back({it->second->image, it->second->memory, req.size});
                g_pool_bytes += req.size;
            } else {
                free_image(it->second->image, it->second->memory);
            }
            it = g_retired_textures.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = g_retired_surfaces.begin(); it != g_retired_surfaces.end();) {
        if (serial > it->first + 4) {
            destroy_surface(it->second.get());
            it = g_retired_surfaces.erase(it);
        } else {
            ++it;
        }
    }
    uint64_t temp_bytes = 0;
    for (auto it = g_temps.begin(); it != g_temps.end();) {
        if (serial > it->second->last_used + 8) {
            destroy_temp(it->second.get());
            it = g_temps.erase(it);
        } else {
            temp_bytes += uint64_t(it->second->iw) * it->second->ih * 4;
            ++it;
        }
    }
    const uint64_t budget = uint64_t(256ull << 20) * uint64_t(std::ceil(res_scale() * res_scale()));
    while (temp_bytes > budget) {
        auto oldest = g_temps.end();
        for (auto it = g_temps.begin(); it != g_temps.end(); ++it) {
            if (serial <= it->second->last_used + 4) continue;
            if (oldest == g_temps.end() || it->second->last_used < oldest->second->last_used) oldest = it;
        }
        if (oldest == g_temps.end()) break;
        temp_bytes -= uint64_t(oldest->second->iw) * oldest->second->ih * 4;
        destroy_temp(oldest->second.get());
        g_temps.erase(oldest);
    }
}

uint32_t clip_w() { return R(SURFACE_CLIP_HORIZONTAL) >> 16; }
uint32_t clip_h() { return R(SURFACE_CLIP_VERTICAL) >> 16; }

std::vector<Surface*> color_targets() {
    std::vector<Surface*> out;
    uint32_t w = clip_w(), h = clip_h();
    if (!w || !h) return out;
    VkFormat f = color_format(R(SURFACE_FORMAT));
    struct Slot { uint32_t bit; uint32_t offset_reg; uint32_t dma_reg; uint32_t pitch_reg; };
    const Slot slots[4] = {{1, SURFACE_COLOR_AOFFSET, CONTEXT_DMA_COLOR_A, 0x020C}, {2, SURFACE_COLOR_BOFFSET, CONTEXT_DMA_COLOR_B, 0x021C}, {4, SURFACE_COLOR_COFFSET, 0x01B4, 0x0280}, {8, SURFACE_COLOR_DOFFSET, 0x01B8, 0x0284}};
    uint32_t mask = color_mask_bits();
    for (const Slot& s : slots) {
        if (!(mask & s.bit)) continue;
        uint32_t dma = R(s.dma_reg);
        bool main = dma == CELL_GCM_LOCATION_LOCAL + 1;
        if (dma != CELL_GCM_LOCATION_LOCAL && !main) continue;
        if (Surface* surf = surface(R(s.offset_reg), f, w, h, false, main, R(s.pitch_reg))) out.push_back(surf);
    }
    return out;
}

Surface* depth_target() {
    uint32_t w = clip_w(), h = clip_h();
    uint32_t dma = R(CONTEXT_DMA_ZETA);
    bool main = dma == CELL_GCM_LOCATION_LOCAL + 1;
    if (!w || !h || !((R(SURFACE_FORMAT) >> 5) & 7) || (dma != CELL_GCM_LOCATION_LOCAL && !main)) return nullptr;
    return surface(R(SURFACE_ZETA_OFFSET), depth_format(R(SURFACE_FORMAT)), w, h, true, main, R(0x022C));
}

void end_pass() {
    if (!g_pass.active) return;
    vkCmdEndRendering(frame_cmd());
    g_pass.active = false;
}

void transition(Surface* s, VkImageLayout to) {
    if (s->layout == to) return;
    end_pass();
    image_barrier(frame_cmd(), s->image, s->aspect, s->layout, to);
    s->layout = to;
}

void begin_pass(const std::vector<Surface*>& colors, Surface* depth, uint32_t w, uint32_t h);

void clear(uint32_t mask) {
    if (g_trace) LOG_INFO("rsx", "trace clear 0x%x color 0x%x zeta 0x%x clip %ux%u target 0x%x scissor 0x%x 0x%x", mask, R(SURFACE_COLOR_AOFFSET), R(SURFACE_ZETA_OFFSET), clip_w(), clip_h(), R(SURFACE_COLOR_TARGET),
                      R(SCISSOR_HORIZONTAL), R(SCISSOR_VERTICAL));
    g_clears++;
    uint32_t w = clip_w(), h = clip_h();
    std::vector<Surface*> colors = color_targets();
    Surface* depth = depth_target();
    if (!w || !h || (colors.empty() && !depth)) return;
    uint32_t sx = R(SCISSOR_HORIZONTAL) & 0xFFFF, sw = R(SCISSOR_HORIZONTAL) >> 16;
    uint32_t sy = R(SCISSOR_VERTICAL) & 0xFFFF, sh = R(SCISSOR_VERTICAL) >> 16;
    uint32_t x0 = std::min(sx, w), y0 = std::min(sy, h), x1 = std::min(sx + sw, w), y1 = std::min(sy + sh, h);
    for (Surface* c : colors) {
        x1 = std::min(x1, c->width);
        y1 = std::min(y1, c->height);
    }
    if (depth) {
        x1 = std::min(x1, depth->width);
        y1 = std::min(y1, depth->height);
    }
    if (x1 <= x0 || y1 <= y0) return;
    std::vector<VkClearAttachment> atts;
    if (mask & 0xF0) {
        uint32_t v = R(COLOR_CLEAR_VALUE);
        VkClearColorValue cv{};
        cv.float32[0] = ((v >> 16) & 0xFF) / 255.0f;
        cv.float32[1] = ((v >> 8) & 0xFF) / 255.0f;
        cv.float32[2] = (v & 0xFF) / 255.0f;
        cv.float32[3] = ((v >> 24) & 0xFF) / 255.0f;
        for (uint32_t i = 0; i < colors.size(); i++) {
            VkClearAttachment a{};
            a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            a.colorAttachment = i;
            a.clearValue.color = cv;
            atts.push_back(a);
            colors[i]->generation++;
            if (colors[i]->main) colors[i]->dirty = true;
        }
    }
    if ((mask & 0x3) && depth) {
        uint32_t v = R(ZSTENCIL_CLEAR_VALUE);
        VkClearAttachment a{};
        if (depth->format == VK_FORMAT_D16_UNORM) {
            a.clearValue.depthStencil.depth = (v & 0xFFFF) / 65535.0f;
            if (mask & 0x1) a.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
        } else {
            a.clearValue.depthStencil.depth = (v >> 8) / 16777215.0f;
            a.clearValue.depthStencil.stencil = v & 0xFF;
            if (mask & 0x1) a.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
            if (mask & 0x2) a.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        if (a.aspectMask) {
            atts.push_back(a);
            depth->generation++;
        }
    }
    if (atts.empty()) return;
    begin_pass(colors, depth, w, h);
    VkClearRect rect{};
    float ps = colors.empty() ? depth->scale : colors[0]->scale;
    uint32_t sx0 = x0 ? up(x0, ps) : 0, sy0 = y0 ? up(y0, ps) : 0;
    rect.rect = {{int32_t(sx0), int32_t(sy0)}, {up(x1, ps) - sx0, up(y1, ps) - sy0}};
    rect.baseArrayLayer = 0;
    rect.layerCount = 1;
    vkCmdClearAttachments(frame_cmd(), static_cast<uint32_t>(atts.size()), atts.data(), 1, &rect);
}

bool init_objects() {
    if (g_ready) return true;
    const VkCtx& c = vk();
    std::vector<VkDescriptorSetLayoutBinding> b;
    for (uint32_t i = 0; i < 4; i++) b.push_back({i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VkShaderStageFlags(i < 2 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT), nullptr});
    for (uint32_t i = 0; i < 4; i++) b.push_back({16 + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr});
    for (uint32_t i = 0; i < 16; i++) b.push_back({32 + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dl.bindingCount = static_cast<uint32_t>(b.size());
    dl.pBindings = b.data();
    if (vkCreateDescriptorSetLayout(c.device, &dl, nullptr, &g_set_layout) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g_set_layout;
    if (vkCreatePipelineLayout(c.device, &pl, nullptr, &g_pipeline_layout) != VK_SUCCESS) return false;
    {
        std::vector<char> blob;
        if (FILE* f = std::fopen("shader_cache/pipelines.bin", "rb")) {
            std::fseek(f, 0, SEEK_END);
            long size = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (size > 0) {
                blob.resize(size_t(size));
                if (std::fread(blob.data(), 1, blob.size(), f) != blob.size()) blob.clear();
            }
            std::fclose(f);
        }
        VkPipelineCacheCreateInfo pc{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        pc.initialDataSize = blob.size();
        pc.pInitialData = blob.empty() ? nullptr : blob.data();
        if (vkCreatePipelineCache(c.device, &pc, nullptr, &g_pipeline_cache) != VK_SUCCESS) {
            pc.initialDataSize = 0;
            pc.pInitialData = nullptr;
            vkCreatePipelineCache(c.device, &pc, nullptr, &g_pipeline_cache);
        }
        LOG_INFO("rsx", "pipeline cache: loaded %zu bytes", blob.size());
    }
    const VkImageViewType types[4] = {VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_3D};
    VkCommandBuffer cmd = frame_cmd();
    for (uint32_t i = 0; i < 4; i++) {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = i == 0 ? VK_IMAGE_TYPE_1D : i == 3 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        ii.flags = i == 2 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = {1, 1, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = i == 2 ? 6 : 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!allocate_image(ii, g_dummy[i].image, g_dummy[i].memory)) return false;
        VkImageMemoryBarrier br{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        br.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        br.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        br.srcQueueFamilyIndex = br.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        br.image = g_dummy[i].image;
        br.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, ii.arrayLayers};
        br.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &br);
        VkClearColorValue zero{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, ii.arrayLayers};
        vkCmdClearColorImage(cmd, g_dummy[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        br.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        br.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        br.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        br.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, nullptr, 0, nullptr, 1, &br);
        g_dummy[i].view = make_view(g_dummy[i].image, VK_FORMAT_R8G8B8A8_UNORM, types[i], VK_IMAGE_ASPECT_COLOR_BIT, {}, 1, ii.arrayLayers);
    }
    for (auto& a : g_imm) {
        a[0] = a[1] = a[2] = 0.0f;
        a[3] = 1.0f;
    }
    g_ready = true;
    return true;
}

VkSamplerAddressMode wrap_mode(uint32_t v) {
    switch (v) {
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 6: case 7: case 8: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

VkCompareOp compare_op(uint32_t v) { return static_cast<VkCompareOp>(v & 7); }

VkCompareOp reversed_compare(uint32_t v) {
    switch (v & 7) {
    case 1: return VK_COMPARE_OP_GREATER;
    case 3: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case 4: return VK_COMPARE_OP_LESS;
    case 6: return VK_COMPARE_OP_LESS_OR_EQUAL;
    default: return static_cast<VkCompareOp>(v & 7);
    }
}

VkSampler sampler(const TextureRegs& t, uint32_t levels, bool shadow) {
    uint64_t key = fnv(&t.address, 4);
    key = fnv(&t.control0, 4, key);
    key = fnv(&t.filter, 4, key);
    key = fnv(&t.border, 4, key);
    uint32_t extra = levels | (shadow ? 0x100 : 0);
    key = fnv(&extra, 4, key);
    auto it = g_samplers.find(key);
    if (it != g_samplers.end()) return it->second;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    uint32_t minf = (t.filter >> 16) & 7, magf = (t.filter >> 24) & 7;
    si.magFilter = magf == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    bool mips = minf >= 3 && minf <= 6 && levels > 1;
    si.minFilter = (minf == 1 || minf == 3 || minf == 5) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    si.mipmapMode = (minf == 5 || minf == 6) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = wrap_mode(t.address & 0xF);
    si.addressModeV = wrap_mode((t.address >> 8) & 0xF);
    si.addressModeW = wrap_mode((t.address >> 16) & 0xF);
    int32_t bias = static_cast<int32_t>(t.filter << 19) >> 19;
    si.mipLodBias = bias / 256.0f;
    float min_lod = ((t.control0 >> 19) & 0xFFF) / 256.0f;
    float max_lod = ((t.control0 >> 7) & 0xFFF) / 256.0f;
    si.minLod = mips ? std::min(min_lod, float(levels - 1)) : 0.0f;
    si.maxLod = mips ? std::min(max_lod, float(levels - 1)) : 0.0f;
    uint32_t aniso = (t.control0 >> 4) & 7;
    static const float aniso_levels[8] = {1, 2, 4, 6, 8, 10, 12, 16};
    if (aniso && vk().features.samplerAnisotropy) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = aniso_levels[aniso];
    }
    if (shadow) {
        si.compareEnable = VK_TRUE;
        si.compareOp = (g_experiment & 256) ? VK_COMPARE_OP_NEVER : reversed_compare(t.address >> 28);
    }
    {
        uint32_t bc = t.border;
        bool white = ((bc >> 16) & 0xFF) >= 0x80 && ((bc >> 8) & 0xFF) >= 0x80 && (bc & 0xFF) >= 0x80;
        bool opaque = (bc >> 24) >= 0x80;
        si.borderColor = white ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : opaque ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(vk().device, &si, nullptr, &s);
    g_samplers[key] = s;
    return s;
}

TextureRegs texture_regs(uint32_t unit) {
    uint32_t b = TEXTURE_BASE + unit * 0x20;
    TextureRegs t;
    t.offset = R(b) & 0x7FFFFFFF;
    t.format = R(b + 0x04);
    t.address = R(b + 0x08);
    t.control0 = R(b + 0x0C);
    t.control1 = R(b + 0x10);
    t.filter = R(b + 0x14);
    t.rect = R(b + 0x18);
    t.border = R(b + 0x1C);
    t.control3 = R(TEXTURE_CONTROL3 + unit * 4);
    return t;
}

Texture* memory_texture(const TextureRegs& t, uint32_t ea) {
    auto key = std::make_tuple(ea, t.format, t.rect, t.control1, t.control3, t.address & 0x00F00000);
    auto it = g_textures.find(key);
    Texture* tex = it != g_textures.end() ? it->second.get() : nullptr;
    if (!tex) {
        auto n = std::make_unique<Texture>();
        if (!describe_texture(t, n->layout)) return nullptr;
        TextureLayout& l = n->layout;
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = l.view_type == VK_IMAGE_VIEW_TYPE_1D ? VK_IMAGE_TYPE_1D : l.view_type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        ii.flags = l.layers == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        ii.format = l.format;
        ii.extent = {l.width, l.height, l.depth};
        ii.mipLevels = l.levels;
        ii.arrayLayers = l.layers;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        n->pool_key = std::make_tuple(uint32_t(ii.imageType), uint32_t(ii.flags), uint32_t(ii.format), ii.extent.width, ii.extent.height, ii.extent.depth, ii.mipLevels, ii.arrayLayers);
        auto pit = g_image_pool.find(n->pool_key);
        if (pit != g_image_pool.end() && !pit->second.empty()) {
            n->image = pit->second.back().image;
            n->memory = pit->second.back().memory;
            g_pool_bytes -= pit->second.back().size;
            pit->second.pop_back();
            g_stats.pool_hits++;
        } else {
            if (!allocate_image(ii, n->image, n->memory)) return nullptr;
            g_stats.image_allocs++;
        }
        n->view = make_view(n->image, l.format, l.view_type, VK_IMAGE_ASPECT_COLOR_BIT, l.mapping, l.levels, l.layers);
        tex = n.get();
        g_textures[key] = std::move(n);
    }
    uint64_t serial = frame_serial();
    tex->last_used = serial;
    if (tex->checked == serial) return tex;
    tex->checked = serial;
    if (!readable(ea, tex->layout.source_size)) return nullptr;
    if (tex->watched) {
        if (tex->img_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && vm::range_epoch(ea, tex->layout.source_size) == tex->epoch) {
            tex->churn = 0;
            g_stats.watch_skips++;
            return tex;
        }
        if (++tex->churn >= 3) tex->nowatch_until = serial + 600;
    }
    bool watch_ok = false;
    uint64_t epoch = vm::watch_enabled() && serial >= tex->nowatch_until ? vm::watch_range(ea, tex->layout.source_size, watch_ok) : 0;
    tex->watched = false;
    const uint8_t* src = vm::ptr(ea);
    uint64_t ht = ticks();
    uint64_t h = fast_hash(src, tex->layout.source_size);
    g_stats.hash_ticks += ticks() - ht;
    g_stats.hash_bytes += tex->layout.source_size;
    g_stats.hash_count++;
    if (h == tex->hash && tex->img_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        tex->epoch = epoch;
        tex->watched = watch_ok;
        return tex;
    }
    RingAlloc staging;
    if (!ring_alloc(tex->layout.decoded_size, 16, staging)) return nullptr;
    decode_texture(src, t, tex->layout, staging.ptr);
    end_pass();
    VkCommandBuffer cmd = frame_cmd();
    VkImageMemoryBarrier br{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    br.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    br.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    br.srcQueueFamilyIndex = br.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    br.image = tex->image;
    br.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, tex->layout.levels, 0, tex->layout.layers};
    br.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    br.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &br);
    std::vector<VkBufferImageCopy> copies;
    for (const TextureRegion& r : tex->layout.regions) {
        VkBufferImageCopy c{};
        c.bufferOffset = staging.offset + r.offset;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, r.level, r.layer, 1};
        c.imageExtent = {r.width, r.height, r.depth};
        copies.push_back(c);
    }
    vkCmdCopyBufferToImage(cmd, staging.buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(copies.size()), copies.data());
    br.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    br.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    br.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    br.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, nullptr, 0, nullptr, 1, &br);
    tex->img_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    tex->hash = h;
    tex->epoch = epoch;
    tex->watched = watch_ok;
    g_stats.uploads++;
    g_stats.upload_frame++;
    g_stats.upload_bytes += tex->layout.source_size;
    return tex;
}

bool is_target(Surface* s) {
    for (uint32_t i = 0; i < g_pass.color_count; i++) {
        if (g_pass.colors[i] == s) return true;
    }
    return g_pass.depth == s;
}

struct Binding {
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    float scale = 1.0f;
};

const char* g_bind_kind = "";



bool is_depth_format(VkFormat f) { return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }

VkImageAspectFlags full_aspect(VkFormat f) {
    if (f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D32_SFLOAT) return VK_IMAGE_ASPECT_DEPTH_BIT;
    if (is_depth_format(f)) return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

TempImage* temp_image(Surface* src, VkFormat format, uint32_t w, uint32_t h, bool redirect) {
    auto key = std::make_tuple(src, w, h, redirect);
    auto it = g_temps.find(key);
    if (it != g_temps.end()) {
        it->second->last_used = frame_serial();
        return it->second.get();
    }
    auto t = std::make_unique<TempImage>();
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {up(w, src->scale), up(h, src->scale), 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               (is_depth_format(format) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (srgb_variant(format) != format) ii.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (!allocate_image(ii, t->image, t->memory)) return nullptr;
    t->format = format;
    t->width = w;
    t->height = h;
    t->scale = src->scale;
    t->iw = ii.extent.width;
    t->ih = ii.extent.height;
    t->last_used = frame_serial();
    TempImage* raw = t.get();
    g_temps[key] = std::move(t);
    return raw;
}

bool texture_srgb(const TextureRegs& t) { return (((t.address >> 20) & 0xF) & 7) == 7; }

VkImageView temp_view(TempImage* t, VkComponentMapping m, bool srgb = false) {
    uint64_t k = fnv(&m, sizeof m) ^ (srgb ? 0x5A5A5A5A5A5A5A5Aull : 0);
    auto it = t->views.find(k);
    if (it != t->views.end()) return it->second;
    VkImageView v = make_view(t->image, srgb ? srgb_variant(t->format) : t->format, VK_IMAGE_VIEW_TYPE_2D, is_depth_format(t->format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 1);
    t->views[k] = v;
    return v;
}

VkDescriptorSetLayout g_redirect_set = VK_NULL_HANDLE;
VkPipelineLayout g_redirect_layout = VK_NULL_HANDLE;
VkPipeline g_redirect_pipeline = VK_NULL_HANDLE;
VkSampler g_point_sampler = VK_NULL_HANDLE;

bool redirect_pipeline() {
    if (g_redirect_pipeline) return true;
    const VkCtx& c = vk();
    const char* vs_src =
        "#version 450\n"
        "void main() { vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
    const char* fs_src =
        "#version 450\n"
        "layout(set = 0, binding = 0) uniform sampler2D depth_tex;\n"
        "layout(location = 0) out vec4 color;\n"
        "void main() {\n"
        "    uint d = uint(clamp(texelFetch(depth_tex, ivec2(gl_FragCoord.xy), 0).r, 0.0, 1.0) * 16777215.0 + 0.5);\n"
        "    color = vec4(float(d & 255u), float((d >> 8) & 255u), float((d >> 16) & 255u), 0.0) / 255.0;\n"
        "}\n";
    std::vector<uint32_t> vs, fs;
    std::string log;
    if (!compile_glsl(ShaderStage::Vertex, vs_src, vs, log) || !compile_glsl(ShaderStage::Fragment, fs_src, fs, log)) {
        LOG_WARN("rsx", "redirect shader failed: %s", log.c_str());
        return false;
    }
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dl.bindingCount = 1;
    dl.pBindings = &b;
    vkCreateDescriptorSetLayout(c.device, &dl, nullptr, &g_redirect_set);
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g_redirect_set;
    vkCreatePipelineLayout(c.device, &pl, nullptr, &g_redirect_layout);
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(c.device, &si, nullptr, &g_point_sampler);
    VkShaderModule vm_ = make_module(vs), fm = make_module(fs);
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vm_, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fm, "main", nullptr};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &att;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &fmt;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri;
    gi.stageCount = 2;
    gi.pStages = stages;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dy;
    gi.layout = g_redirect_layout;
    return vkCreateGraphicsPipelines(c.device, g_pipeline_cache, 1, &gi, nullptr, &g_redirect_pipeline) == VK_SUCCESS;
}

VkImageView depth_view(Surface* s) {
    VkComponentMapping m{};
    uint64_t mk = fnv(&m, sizeof m);
    auto it = s->sample_views.find(mk);
    if (it != s->sample_views.end()) return it->second;
    VkImageView v = make_view(s->image, s->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, m, 1, 1);
    s->sample_views[mk] = v;
    return v;
}

void redirect_pass(Surface* s, TempImage* t, uint32_t w, uint32_t h) {
    transition(s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkCommandBuffer cmd = frame_cmd();
    image_barrier(cmd, t->image, VK_IMAGE_ASPECT_COLOR_BIT, t->layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkImageView target = temp_view(t, VkComponentMapping{});
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = target;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {t->iw, t->ih}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_redirect_pipeline);
    VkDescriptorImageInfo ii{g_point_sampler, depth_view(s), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, nullptr, nullptr};
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_redirect_layout, 0, 1, &wr);
    VkViewport viewport{0, 0, float(t->iw), float(t->ih), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {t->iw, t->ih}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    image_barrier(cmd, t->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

TempImage* copy_to_temp(Surface* s, uint32_t w, uint32_t h, bool redirect) {
    if (g_experiment & 2) return nullptr;
    if ((g_experiment & 4) && redirect) return nullptr;
    if ((g_experiment & 8) && !redirect) return nullptr;
    TempImage* t = temp_image(s, redirect ? VK_FORMAT_R8G8B8A8_UNORM : s->format, w, h, redirect);
    if (!t) return nullptr;
    if (t->generation == s->generation) return t;
    t->generation = s->generation;
    g_stats.copies++;
    g_stats.copy_bytes += uint64_t(w) * h * 4;
    if (redirect) g_stats.redirects++;
    if (redirect) {
        if (!redirect_pipeline()) return nullptr;
        redirect_pass(s, t, w, h);
        return t;
    }
    transition(s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkCommandBuffer cmd = frame_cmd();
    VkImageAspectFlags taspect = full_aspect(t->format);
    image_barrier(cmd, t->image, taspect, t->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (redirect) {
        RingAlloc staging;
        if (!ring_alloc(uint64_t(t->iw) * t->ih * 4, 16, staging)) return nullptr;
        VkBufferImageCopy c{};
        c.bufferOffset = staging.offset;
        c.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        c.imageExtent = {std::min(t->iw, s->iw), std::min(t->ih, s->ih), 1};
        vkCmdCopyImageToBuffer(cmd, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &c);
        VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.buffer = staging.buffer;
        bb.offset = staging.offset;
        bb.size = uint64_t(t->iw) * t->ih * 4;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &bb, 0, nullptr);
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    } else {
        VkImageCopy c{};
        c.srcSubresource = {s->aspect, 0, 0, 1};
        c.dstSubresource = {taspect, 0, 0, 1};
        c.extent = {std::min(t->iw, s->iw), std::min(t->ih, s->ih), 1};
        vkCmdCopyImage(cmd, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    }
    image_barrier(cmd, t->image, taspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return t;
}

Binding bind_texture_impl(uint32_t unit, uint32_t dim, const std::vector<Surface*>& colors, Surface* depth);

void dump_texture(const TextureRegs& t, uint32_t ea) {
    static std::set<uint64_t> done;
    static int pass_id = 0;
    static uint64_t last_presents = 0;
    pass_id = g_trace_serial;
    uint64_t key = (uint64_t(ea) << 32) ^ t.format ^ (uint64_t(t.rect) << 8) ^ (uint64_t(pass_id) << 60);
    if (!done.insert(key).second) return;
    (void)last_presents;
    TextureLayout l;
    if (!describe_texture(t, l) || !readable(ea, l.source_size)) return;
    std::vector<uint8_t> buf(l.decoded_size);
    decode_texture(vm::ptr(ea), t, l, buf.data());
    const TextureRegion& r = l.regions[0];
    char fn[96];
    bool bc = l.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || l.format == VK_FORMAT_BC1_RGBA_SRGB_BLOCK || l.format == VK_FORMAT_BC2_UNORM_BLOCK || l.format == VK_FORMAT_BC2_SRGB_BLOCK ||
              l.format == VK_FORMAT_BC3_UNORM_BLOCK || l.format == VK_FORMAT_BC3_SRGB_BLOCK;
    if (bc) {
        uint32_t fourcc = (l.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || l.format == VK_FORMAT_BC1_RGBA_SRGB_BLOCK) ? 0x31545844u
                          : (l.format == VK_FORMAT_BC2_UNORM_BLOCK || l.format == VK_FORMAT_BC2_SRGB_BLOCK)     ? 0x33545844u
                                                                                                                 : 0x35545844u;
        uint32_t bsize = fourcc == 0x31545844u ? 8 : 16;
        uint32_t bytes = ((r.width + 3) / 4) * ((r.height + 3) / 4) * bsize;
        std::snprintf(fn, sizeof fn, "tex_%08x_%ux%u_%02x_t%d.dds", ea, r.width, r.height, (t.format >> 8) & 0xFF, int(std::time(nullptr) % 100000));
        if (FILE* f = std::fopen(fn, "wb")) {
            uint32_t h[32] = {};
            h[0] = 0x20534444u;
            h[1] = 124;
            h[2] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000;
            h[3] = r.height;
            h[4] = r.width;
            h[5] = bytes;
            h[19] = 32;
            h[20] = 0x4;
            h[21] = fourcc;
            h[27] = 0x1000;
            std::fwrite(h, 4, 32, f);
            std::fwrite(buf.data() + r.offset, 1, bytes, f);
            std::fclose(f);
        }
    } else if (l.format == VK_FORMAT_B8G8R8A8_UNORM || l.format == VK_FORMAT_B8G8R8A8_SRGB) {
        std::snprintf(fn, sizeof fn, "tex_%08x_%ux%u_%02x.ppm", ea, r.width, r.height, (t.format >> 8) & 0xFF);
        if (FILE* f = std::fopen(fn, "wb")) {
            std::fprintf(f, "P6\n%u %u\n255\n", r.width, r.height);
            for (uint32_t i = 0; i < r.width * r.height; i++) {
                const uint8_t* px = buf.data() + r.offset + i * 4;
                uint8_t rgb[3] = {px[2], px[1], px[0]};
                std::fwrite(rgb, 1, 3, f);
            }
            std::fclose(f);
        }
    }
}

Binding bind_texture(uint32_t unit, uint32_t dim, const std::vector<Surface*>& colors, Surface* depth) {
    g_bind_kind = "dummy";
    Binding b = bind_texture_impl(unit, dim, colors, depth);
    if (trace::on(trace::TEX)) {
        TextureRegs t = texture_regs(unit);
        static const char* kinds[] = {"dummy", "memory", "depth redirect", "feedback copy", "surface region", "depth surface", "color surface"};
        uint32_t k = 0;
        for (uint32_t i = 0; i < 7; i++) if (std::strcmp(g_bind_kind, kinds[i]) == 0) k = i;
        trace::emit(trace::T_TEX, unit | (k << 8), lv2::rsx_address(t.offset, (t.format & 3) - 1), t.format, t.rect, t.control0);
    }
    if (g_trace) {
        TextureRegs t = texture_regs(unit);
        uint32_t tea = lv2::rsx_address(t.offset, (t.format & 3) - 1);
        if (std::getenv("GOWA_TEX_DUMP") && tea && std::string(g_bind_kind) == "memory") dump_texture(t, tea);
        uint32_t w0 = tea && readable(tea, 64) ? vm::read32(tea) : 0, w1 = tea && readable(tea, 64) ? vm::read32(tea + 4) : 0, w8 = tea && readable(tea, 64) ? vm::read32(tea + 32) : 0;
        LOG_INFO("rsx", "  tex%u off 0x%x fmt 0x%x rect %ux%u c0 0x%x dim %u -> %s data %08x %08x %08x addr 0x%x c1 0x%x c3 0x%x filter 0x%x border 0x%x", unit, t.offset, t.format, t.rect >> 16, t.rect & 0xFFFF, t.control0, dim, g_bind_kind, w0, w1, w8, t.address, t.control1, t.control3, t.filter, t.border);
    }
    return b;
}

Binding bind_texture_impl(uint32_t unit, uint32_t dim, const std::vector<Surface*>& colors, Surface* depth) {
    Binding b{g_dummy[dim].view, VK_NULL_HANDLE};
    TextureRegs t = texture_regs(unit);
    b.sampler = sampler(t, 1, false);
    if (!(t.control0 >> 31)) return b;
    uint32_t loc = (t.format & 3) - 1;
    uint32_t code = ((t.format >> 8) & 0xFF) & ~0x60u;
    bool depth_fmt = code >= 0x90 && code <= 0x93;
    if ((loc == 0 || loc == 1) && dim == 1) {
        uint32_t w = t.rect >> 16, h = t.rect & 0xFFFF;
        Surface* exact = nullptr;
        Surface* larger = nullptr;
        Surface* redirect = nullptr;
        for (Surface* s : surfaces_at(t.offset)) {
            if (s->offset != t.offset || s->main != (loc == 1) || s->width < w || s->height < h) continue;
            if (s->depth() == depth_fmt) {
                if (s->width == w && s->height == h) exact = s;
                else if (!larger) larger = s;
            } else if (s->depth() && !depth_fmt && s->format == VK_FORMAT_D24_UNORM_S8_UINT) {
                redirect = s;
            }
        }
        Surface* src = exact ? exact : larger ? larger : redirect;
        bool target = src && (std::find(colors.begin(), colors.end(), src) != colors.end() || src == depth);
        if (src && (src != exact || target || src == redirect)) {
            TempImage* tmp = copy_to_temp(src, w, h, src == redirect);
            if (!tmp) return b;
            VkComponentMapping m;
            if (src == redirect) {
                static const VkComponentSwizzle base[4] = {VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO};
                m = remap_mapping(base, true, t.control1);
            } else {
                m = texture_mapping(0x85, t.control1);
            }
            g_bind_kind = src == redirect ? "depth redirect" : target ? "feedback copy" : "surface region";
            bool depth_copy = src != redirect && src->depth();
            if (depth_copy) m = VkComponentMapping{};
            b.view = temp_view(tmp, m, !depth_copy && src != redirect && texture_srgb(t));
            b.scale = tmp->scale;
            uint32_t tzf = (t.address >> 28) & 7;
            b.sampler = sampler(t, 1, depth_copy && tzf != 0 && tzf != 7);
            return b;
        }
        Surface* best = exact;
        if (best) {
            g_bind_kind = best->depth() ? "depth surface" : "color surface";
            transition(best, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            VkComponentMapping m = best->depth() ? VkComponentMapping{} : texture_mapping(0x85, t.control1);
            bool srgb = !best->depth() && texture_srgb(t) && srgb_variant(best->format) != best->format;
            uint64_t mk = fnv(&m, sizeof m) ^ (srgb ? 0x5A5A5A5A5A5A5A5Aull : 0);
            auto it = best->sample_views.find(mk);
            VkImageView v;
            if (it != best->sample_views.end()) {
                v = it->second;
            } else {
                v = make_view(best->image, srgb ? srgb_variant(best->format) : best->format, VK_IMAGE_VIEW_TYPE_2D, best->depth() ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 1);
                best->sample_views[mk] = v;
            }
            uint32_t zf = (t.address >> 28) & 7;
            b.view = v;
            b.scale = best->scale;
            b.sampler = sampler(t, 1, best->depth() && zf != 0 && zf != 7);
            return b;
        }
    }
    uint32_t ea = lv2::rsx_address(t.offset, loc);
    if (!ea) return b;
    uint64_t tt = ticks();
    Texture* tex = memory_texture(t, ea);
    g_stats.tex_ticks += ticks() - tt;
    if (!tex) return b;
    VkImageViewType want = dim == 0 ? VK_IMAGE_VIEW_TYPE_1D : dim == 2 ? VK_IMAGE_VIEW_TYPE_CUBE : dim == 3 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    if (tex->layout.view_type != want) return b;
    b.view = tex->view;
    g_bind_kind = "memory";
    b.sampler = sampler(t, tex->layout.levels, false);
    return b;
}

void decode_attribute(uint32_t type, uint32_t size, const uint8_t* p, float* out) {
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    switch (type) {
    case 1:
        for (uint32_t i = 0; i < size; i++) out[i] = (int16_t((p[i * 2] << 8) | p[i * 2 + 1]) + 0.5f) / 32767.5f;
        break;
    case 2:
        for (uint32_t i = 0; i < size; i++) {
            uint32_t v = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
            std::memcpy(&out[i], &v, 4);
        }
        break;
    case 3:
        for (uint32_t i = 0; i < size; i++) out[i] = half_to_float(uint16_t((p[i * 2] << 8) | p[i * 2 + 1]));
        break;
    case 4:
        for (uint32_t i = 0; i < size; i++) out[i] = p[i] / 255.0f;
        break;
    case 5:
        for (uint32_t i = 0; i < size; i++) out[i] = float(int16_t((p[i * 2] << 8) | p[i * 2 + 1]));
        break;
    case 6: {
        uint32_t v = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        int32_t x = int32_t(v << 21) >> 21, y = int32_t(v << 10) >> 21, z = int32_t(v) >> 22;
        out[0] = (x * 32) / 32767.0f;
        out[1] = (y * 32) / 32767.0f;
        out[2] = (z * 64) / 32767.0f;
        break;
    }
    default:
        for (uint32_t i = 0; i < size; i++) out[i] = float(p[i]);
        break;
    }
}

#define SPU_VDEC_INLINE inline __attribute__((always_inline))
template <uint32_t Bytes>
SPU_VDEC_INLINE __m128i load_attr(const uint8_t* p) {
    if constexpr (Bytes == 16) return _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    else if constexpr (Bytes == 8) return _mm_loadl_epi64(reinterpret_cast<const __m128i*>(p));
    else if constexpr (Bytes == 4) {
        int32_t v;
        std::memcpy(&v, p, 4);
        return _mm_cvtsi32_si128(v);
    } else {
        alignas(16) uint8_t t[16] = {};
        std::memcpy(t, p, Bytes);
        return _mm_load_si128(reinterpret_cast<const __m128i*>(t));
    }
}

template <uint32_t Size>
SPU_VDEC_INLINE __m128 keep_lanes(__m128 v) {
    const __m128 def = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);
    if constexpr (Size >= 4) return v;
    else return _mm_blend_ps(def, v, (1 << Size) - 1);
}

template <uint32_t Type, uint32_t Size>
void decode_run(const uint8_t* p, uint32_t stride, uint32_t count, float* out) {
    const __m128i swap32 = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    const __m128i swap16 = _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
    for (uint32_t v = 0; v < count; v++, p += stride, out += 4) {
        __m128 f;
        if constexpr (Type == 2) {
            __m128i x = load_attr<Size * 4>(p);
            f = _mm_castsi128_ps(_mm_shuffle_epi8(x, swap32));
        } else if constexpr (Type == 1) {
            __m128i x = _mm_cvtepi16_epi32(_mm_shuffle_epi8(load_attr<Size * 2>(p), swap16));
            f = _mm_div_ps(_mm_add_ps(_mm_cvtepi32_ps(x), _mm_set1_ps(0.5f)), _mm_set1_ps(32767.5f));
        } else if constexpr (Type == 3) {
            f = _mm_cvtph_ps(_mm_shuffle_epi8(load_attr<Size * 2>(p), swap16));
        } else {
            __m128i x = _mm_cvtepu8_epi32(load_attr<Size>(p));
            f = _mm_div_ps(_mm_cvtepi32_ps(x), _mm_set1_ps(255.0f));
        }
        _mm_storeu_ps(out, keep_lanes<Size>(f));
    }
}

template <uint32_t Type>
bool decode_typed(uint32_t size, const uint8_t* p, uint32_t stride, uint32_t count, float* out) {
    switch (size) {
    case 1: decode_run<Type, 1>(p, stride, count, out); return true;
    case 2: decode_run<Type, 2>(p, stride, count, out); return true;
    case 3: decode_run<Type, 3>(p, stride, count, out); return true;
    case 4: decode_run<Type, 4>(p, stride, count, out); return true;
    default: return false;
    }
}

void decode_vertices(uint32_t type, uint32_t size, const uint8_t* p, uint32_t stride, uint32_t count, float* out) {
    bool done = false;
    switch (type) {
    case 1: done = decode_typed<1>(size, p, stride, count, out); break;
    case 2: done = decode_typed<2>(size, p, stride, count, out); break;
    case 3: done = decode_typed<3>(size, p, stride, count, out); break;
    case 4: done = decode_typed<4>(size, p, stride, count, out); break;
    default: break;
    }
    if (done) return;
    for (uint32_t v = 0; v < count; v++, p += stride, out += 4) decode_attribute(type, size, p, out);
}

uint32_t attribute_bytes(uint32_t type, uint32_t size) {
    switch (type) {
    case 2: return 4 * size;
    case 1: case 3: case 5: return 2 * size;
    case 6: return 4;
    default: return size;
    }
}

uint32_t inline_host_bytes(uint32_t type, uint32_t size) {
    switch (type) {
    case 2: return 4 * size;
    case 1: case 3: case 5: return size == 3 ? 8 : 2 * size;
    case 6: return 4;
    default: return size == 3 ? 4 : size;
    }
}

VkBlendFactor blend_factor(uint32_t v) {
    switch (v) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 0x300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp blend_op(uint32_t v) {
    switch (v) {
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: case 0xF005: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
    }
}

VkStencilOp stencil_op(uint32_t v) {
    switch (v) {
    case 0: return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

uint32_t color_write_mask(uint32_t target) {
    uint32_t m = 0;
    if (target == 0) {
        uint32_t v = R(COLOR_MASK);
        if (v & 0x00FF0000) m |= VK_COLOR_COMPONENT_R_BIT;
        if (v & 0x0000FF00) m |= VK_COLOR_COMPONENT_G_BIT;
        if (v & 0x000000FF) m |= VK_COLOR_COMPONENT_B_BIT;
        if (v & 0xFF000000) m |= VK_COLOR_COMPONENT_A_BIT;
    } else {
        uint32_t n = (R(COLOR_MASK_MRT) >> (4 * target)) & 0xF;
        if (n & 2) m |= VK_COLOR_COMPONENT_R_BIT;
        if (n & 4) m |= VK_COLOR_COMPONENT_G_BIT;
        if (n & 8) m |= VK_COLOR_COMPONENT_B_BIT;
        if (n & 1) m |= VK_COLOR_COMPONENT_A_BIT;
    }
    return m;
}

struct PipelineKey {
    uint64_t vp;
    uint64_t fp;
    uint32_t inputs;
    uint32_t color_formats[4];
    uint32_t color_count;
    uint32_t depth_format;
    uint32_t blend_mask;
    uint32_t sfactor;
    uint32_t dfactor;
    uint32_t equation;
    uint32_t write_masks;
    uint32_t logic;
    uint32_t polygon_mode;
    uint32_t topology_class;
};

VkResult create_pipeline(const PipelineKey& k, VkShaderModule vs, VkShaderModule fs, VkPipelineCreateFlags flags, VkPipeline& p) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};
    std::vector<VkVertexInputBindingDescription> binds;
    std::vector<VkVertexInputAttributeDescription> attrs;
    for (uint32_t i = 0; i < 16; i++) {
        if (!(k.inputs & (1u << i))) continue;
        binds.push_back({i, 16, VK_VERTEX_INPUT_RATE_VERTEX});
        attrs.push_back({i, i, VK_FORMAT_R32G32B32A32_SFLOAT, 0});
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = static_cast<uint32_t>(binds.size());
    vi.pVertexBindingDescriptions = binds.data();
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = k.topology_class == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : k.topology_class == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = vk().features.fillModeNonSolid ? static_cast<VkPolygonMode>(k.polygon_mode) : VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState att[4]{};
    for (uint32_t i = 0; i < k.color_count; i++) {
        att[i].blendEnable = (k.blend_mask >> i) & 1;
        att[i].srcColorBlendFactor = blend_factor(k.sfactor & 0xFFFF);
        att[i].srcAlphaBlendFactor = blend_factor(k.sfactor >> 16);
        att[i].dstColorBlendFactor = blend_factor(k.dfactor & 0xFFFF);
        att[i].dstAlphaBlendFactor = blend_factor(k.dfactor >> 16);
        att[i].colorBlendOp = blend_op(k.equation & 0xFFFF);
        att[i].alphaBlendOp = blend_op(k.equation >> 16);
        att[i].colorWriteMask = (k.write_masks >> (4 * i)) & 0xF;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = k.color_count;
    cb.pAttachments = att;
    if (k.logic && vk().features.logicOp) {
        cb.logicOpEnable = VK_TRUE;
        cb.logicOp = static_cast<VkLogicOp>(k.logic & 0xF);
        for (uint32_t i = 0; i < k.color_count; i++) att[i].blendEnable = VK_FALSE;
    }
    std::vector<VkDynamicState> dyn = {VK_DYNAMIC_STATE_VIEWPORT,
                                       VK_DYNAMIC_STATE_SCISSOR,
                                       VK_DYNAMIC_STATE_LINE_WIDTH,
                                       VK_DYNAMIC_STATE_DEPTH_BIAS,
                                       VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                       VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                       VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                       VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                       VK_DYNAMIC_STATE_CULL_MODE,
                                       VK_DYNAMIC_STATE_FRONT_FACE,
                                       VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
                                       VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE,
                                       VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                                       VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                                       VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                                       VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
                                       VK_DYNAMIC_STATE_STENCIL_OP,
                                       VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
                                       VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
    if (vk().features.depthBounds) {
        dyn.push_back(VK_DYNAMIC_STATE_DEPTH_BOUNDS);
        dyn.push_back(VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE);
    }
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = static_cast<uint32_t>(dyn.size());
    dy.pDynamicStates = dyn.data();
    VkFormat formats[4];
    for (uint32_t i = 0; i < k.color_count; i++) formats[i] = static_cast<VkFormat>(k.color_formats[i]);
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = k.color_count;
    ri.pColorAttachmentFormats = formats;
    ri.depthAttachmentFormat = static_cast<VkFormat>(k.depth_format);
    ri.stencilAttachmentFormat = k.depth_format == VK_FORMAT_D24_UNORM_S8_UINT ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri;
    gi.stageCount = 2;
    gi.pStages = stages;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dy;
    gi.layout = g_pipeline_layout;
    gi.flags = flags;
    p = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(vk().device, g_pipeline_cache, 1, &gi, nullptr, &p);
    if (r != VK_SUCCESS) p = VK_NULL_HANDLE;
    if (r != VK_SUCCESS && r != VK_PIPELINE_COMPILE_REQUIRED) LOG_WARN("rsx", "pipeline creation failed (%d)", static_cast<int>(r));
    return r;
}

VkPipeline pipeline(const PipelineKey& k, VkShaderModule vs, VkShaderModule fs) {
    uint64_t h = fnv(&k, sizeof k);
    auto it = g_pipelines.find(h);
    if (it != g_pipelines.end()) return it->second.ready.load(std::memory_order_acquire) ? it->second.pipeline : VK_NULL_HANDLE;
    PipelineEntry& e = g_pipelines[h];
    CompileTimer ct;
    g_compile_frame.pipelines++;
    g_stats.pipelines++;
    if (async_compile()) {
        if (vk().cache_control && create_pipeline(k, vs, fs, VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT, e.pipeline) == VK_SUCCESS) {
            e.ready.store(true, std::memory_order_release);
            return e.pipeline;
        }
        g_compile_pool.submit([&e, k, vs, fs] {
            create_pipeline(k, vs, fs, 0, e.pipeline);
            e.ready.store(true, std::memory_order_release);
        });
        return VK_NULL_HANDLE;
    }
    create_pipeline(k, vs, fs, 0, e.pipeline);
    e.ready.store(true, std::memory_order_release);
    return e.pipeline;
}

void build_indices(std::vector<uint32_t>& raw, bool& inline_draw, uint32_t& inline_stride) {
    inline_draw = false;
    if (!g_inline.empty()) {
        inline_stride = 0;
        for (uint32_t i = 0; i < 16; i++) {
            uint32_t f = R(VERTEX_ARRAY_FORMAT + i * 4);
            uint32_t size = (f >> 4) & 0xF;
            if (size) inline_stride += inline_host_bytes(f & 7, size);
        }
        if (!inline_stride) return;
        uint32_t count = static_cast<uint32_t>(g_inline.size() * 4 / inline_stride);
        for (uint32_t i = 0; i < count; i++) raw.push_back(i);
        inline_draw = true;
        return;
    }
    uint32_t base_index = R(VERTEX_BASE_INDEX);
    if (g_indexed) {
        uint32_t dma = R(INDEX_ARRAY_DMA);
        bool u16 = ((dma >> 4) & 0xF) == 1;
        uint32_t ea = lv2::rsx_address(R(INDEX_ARRAY_ADDRESS), dma & 0xF);
        if (!ea) return;
        ea &= u16 ? ~1u : ~3u;
        uint32_t span = 0;
        for (auto [first, count] : g_ranges) span = std::max(span, first + count);
        if (!readable(ea, uint64_t(span) * (u16 ? 2 : 4))) return;
        bool restart = R(RESTART_INDEX_ENABLE) != 0;
        uint32_t restart_value = R(RESTART_INDEX);
        if (u16 && restart_value > 0xFFFF) restart = false;
        for (auto [first, count] : g_ranges) {
            if (!raw.empty()) raw.push_back(RESTART);
            size_t at = raw.size();
            raw.resize(at + count);
            uint32_t* dst = raw.data() + at;
            if (u16) {
                const uint8_t* src = vm::ptr(ea + first * 2);
                for (uint32_t n = 0; n < count; n++) {
                    uint16_t w;
                    std::memcpy(&w, src + n * 2, 2);
                    uint32_t v = __builtin_bswap16(w);
                    dst[n] = restart && v == restart_value ? RESTART : (v + base_index) & 0xFFFFF;
                }
            } else {
                const uint8_t* src = vm::ptr(ea + first * 4);
                for (uint32_t n = 0; n < count; n++) {
                    uint32_t w;
                    std::memcpy(&w, src + n * 4, 4);
                    uint32_t v = __builtin_bswap32(w);
                    dst[n] = restart && v == restart_value ? RESTART : (v + base_index) & 0xFFFFF;
                }
            }
        }
        return;
    }
    if (!g_elements.empty()) {
        for (uint32_t v : g_elements) raw.push_back((v + base_index) & 0xFFFFF);
        return;
    }
    for (auto [first, count] : g_ranges) {
        if (!raw.empty()) raw.push_back(RESTART);
        for (uint32_t n = 0; n < count; n++) raw.push_back(first + n);
    }
}

VkPrimitiveTopology expand(uint32_t prim, const std::vector<uint32_t>& in, std::vector<uint32_t>& out, bool& restart) {
    restart = false;
    out.clear();
    const uint32_t* d = in.data();
    size_t n = in.size(), pos = 0;
    auto next = [&](size_t& b, size_t& e) {
        while (pos < n && d[pos] == RESTART) pos++;
        if (pos >= n) return false;
        b = pos;
        while (pos < n && d[pos] != RESTART) pos++;
        e = pos;
        return true;
    };
    auto append = [&](size_t b, size_t e, bool keep_restart) {
        if (e <= b) return;
        if (!out.empty() && keep_restart) {
            out.push_back(RESTART);
            restart = true;
        }
        out.insert(out.end(), d + b, d + e);
    };
    size_t b, e;
    switch (prim) {
    case 1:
        out.reserve(n);
        while (next(b, e)) append(b, e, false);
        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 2:
        out.reserve(n);
        while (next(b, e)) append(b, b + ((e - b) & ~size_t(1)), false);
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 3:
        out.reserve(n + n / 2 + 2);
        while (next(b, e)) {
            append(b, e, true);
            if (e - b > 1) out.push_back(d[b]);
        }
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 4:
        out.reserve(n);
        while (next(b, e)) append(b, e, true);
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 5:
        out.reserve(n);
        while (next(b, e)) append(b, e - (e - b) % 3, false);
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case 6: case 9:
        out.reserve(n);
        while (next(b, e)) append(b, e, true);
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 7: case 10:
        out.reserve(n);
        while (next(b, e)) append(b, e, true);
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case 8:
        out.reserve(n + n / 2);
        while (next(b, e)) {
            for (size_t i = b; i + 3 < e; i += 4) {
                uint32_t q[6] = {d[i], d[i + 1], d[i + 2], d[i + 2], d[i + 3], d[i]};
                out.insert(out.end(), q, q + 6);
            }
        }
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    default: return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    }
}

uint32_t topology_class(VkPrimitiveTopology t) {
    if (t == VK_PRIMITIVE_TOPOLOGY_POINT_LIST) return 0;
    if (t == VK_PRIMITIVE_TOPOLOGY_LINE_LIST || t == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP) return 1;
    return 2;
}

bool upload_uniform(const void* data, size_t size, VkDescriptorBufferInfo& out) {
    RingAlloc a;
    if (!ring_alloc((size + 15) & ~size_t(15), 256, a)) return false;
    std::memcpy(a.ptr, data, size);
    out = {a.buffer, a.offset, (size + 15) & ~size_t(15)};
    return true;
}

void begin_pass(const std::vector<Surface*>& colors, Surface* depth, uint32_t w, uint32_t h) {
    bool same = g_pass.active && g_pass.color_count == colors.size() && g_pass.depth == depth && g_pass.width == w && g_pass.height == h;
    for (uint32_t i = 0; same && i < colors.size(); i++) same = g_pass.colors[i] == colors[i];
    if (same) return;
    end_pass();
    for (Surface* s : colors) {
        transition(s, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        s->generation++;
        if (s->main) s->dirty = true;
    }
    if (depth) {
        transition(depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        depth->generation++;
    }
    VkRenderingAttachmentInfo ca[4]{};
    for (size_t i = 0; i < colors.size(); i++) {
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        ca[i].imageView = colors[i]->view;
        ca[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        g_pass.colors[i] = colors[i];
    }
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (depth) {
        da.imageView = depth->view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    float ps = colors.empty() ? (depth ? depth->scale : 1.0f) : colors[0]->scale;
    ri.renderArea = {{0, 0}, {up(w, ps), up(h, ps)}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = static_cast<uint32_t>(colors.size());
    ri.pColorAttachments = ca;
    ri.pDepthAttachment = depth ? &da : nullptr;
    ri.pStencilAttachment = depth && depth->format == VK_FORMAT_D24_UNORM_S8_UINT ? &da : nullptr;
    vkCmdBeginRendering(frame_cmd(), &ri);
    g_pass.active = true;
    g_pass.color_count = static_cast<uint32_t>(colors.size());
    g_pass.depth = depth;
    g_pass.width = w;
    g_pass.height = h;
}

std::map<int, uint64_t> g_skip_lines;

bool skip(int line) {
    g_skip_lines[line]++;
    if (g_trace) LOG_INFO("rsx", "  draw skipped at line %d", line);
    return false;
}

bool g_mv_done = false;
std::string g_mv_hudless;
void mv_draw(const std::vector<Surface*>& colors, uint32_t w, uint32_t h);

bool draw() {
    VkCommandBuffer cmd = frame_cmd();
    if (!init_objects()) return skip(__LINE__);
    uint64_t ph = ticks();
    VpEntry* vp = vertex_program();
    if (!vp || !vp->ready.load(std::memory_order_acquire) || !vp->ok) return skip(__LINE__);
    uint32_t fp_ea = fp_address();
    FpEntry* fp = fragment_program(fp_ea);
    if (!fp || !fp->ready.load(std::memory_order_acquire) || !fp->ok) return skip(__LINE__);
    uint32_t w = clip_w(), h = clip_h();
    if (!w || !h) return skip(__LINE__);
    std::vector<Surface*> colors = color_targets();
    Surface* depth = depth_target();
    if (colors.empty() && !depth) {
        static int logged = 0;
        if (logged++ < 10) LOG_INFO("rsx", "no target: color target 0x%x dmaA 0x%x dmaB 0x%x zeta dma 0x%x fmt 0x%x offA 0x%x offZ 0x%x pitchA 0x%x clip %ux%u", R(SURFACE_COLOR_TARGET), R(CONTEXT_DMA_COLOR_A),
                                    R(CONTEXT_DMA_COLOR_B), R(CONTEXT_DMA_ZETA), R(SURFACE_FORMAT), R(SURFACE_COLOR_AOFFSET), R(SURFACE_ZETA_OFFSET), R(0x020C), w, h);
        return skip(__LINE__);
    }
    static std::vector<uint32_t> raw;
    raw.clear();
    bool inline_draw = false;
    uint32_t inline_stride = 0;
    g_stats.phase[0] += ticks() - ph;
    ph = ticks();
    build_indices(raw, inline_draw, inline_stride);
    static std::vector<uint32_t> indices;
    bool restart = false;
    VkPrimitiveTopology topo = expand(g_prim, raw, indices, restart);
    if (topo == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM || indices.empty()) return skip(__LINE__);
    uint32_t vmin = ~0u, vmax = 0;
    {
        const uint32_t* ip = indices.data();
        size_t n = indices.size();
        for (size_t i = 0; i < n; i++) {
            uint32_t v = ip[i];
            vmin = std::min(vmin, v);
            vmax = std::max(vmax, v == RESTART ? 0u : v);
        }
    }
    if (vmin > vmax || vmax - vmin > (1u << 20)) return skip(__LINE__);
    uint32_t vcount = vmax - vmin + 1;

    uint32_t sx = R(SCISSOR_HORIZONTAL) & 0xFFFF, sw = R(SCISSOR_HORIZONTAL) >> 16;
    uint32_t sy = R(SCISSOR_VERTICAL) & 0xFFFF, sh = R(SCISSOR_VERTICAL) >> 16;
    uint32_t x0 = std::min(sx, w), y0 = std::min(sy, h), x1 = std::min(sx + sw, w), y1 = std::min(sy + sh, h);
    if (x1 <= x0 || y1 <= y0) return skip(__LINE__);

    g_stats.phase[1] += ticks() - ph;
    ph = ticks();
    VkWriteDescriptorSet writes[24]{};
    VkDescriptorImageInfo images[20]{};
    uint32_t nw = 0, ni = 0;
    float tex_scales[16];
    for (float& v : tex_scales) v = 1.0f;
    uint64_t bind_start = ticks();
    for (uint32_t u = 0; u < 16; u++) {
        if (!(fp->textures & (1u << u))) continue;
        Binding b = bind_texture(u, (fp->dims >> (u * 2)) & 3, colors, depth);
        tex_scales[u] = b.scale;
        images[ni] = {b.sampler, b.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 32 + u, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[ni], nullptr, nullptr};
        nw++;
        ni++;
    }
    g_stats.phase[7] += ticks() - bind_start;
    for (uint32_t t = 0; t < 4; t++) {
        TextureRegs dummy_regs;
        images[ni] = {sampler(dummy_regs, 1, false), g_dummy[1].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 16 + t, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[ni], nullptr, nullptr};
        nw++;
        ni++;
    }

    uint64_t serial = frame_serial();
    VkDescriptorBufferInfo ubo[4];
    if (g_vp_constants_dirty || g_vp_constants_serial != serial) {
        if (!ring_alloc(sizeof g_vp_constants, 256, g_vp_constants_alloc)) return skip(__LINE__);
        std::memcpy(g_vp_constants_alloc.ptr, g_vp_constants, sizeof g_vp_constants);
        g_vp_constants_dirty = false;
        g_vp_constants_serial = serial;
    }
    ubo[0] = {g_vp_constants_alloc.buffer, g_vp_constants_alloc.offset, sizeof g_vp_constants};
    struct VertexState {
        float scale[4];
        float offset[4];
        float surface[4];
        uint32_t flags[4];
    } vs{};
    for (uint32_t i = 0; i < 4; i++) {
        vs.scale[i] = RF(VIEWPORT_SCALE + i * 4);
        vs.offset[i] = RF(VIEWPORT_OFFSET + i * 4);
    }
    vs.surface[0] = float(w);
    vs.surface[1] = float(h);
    vs.surface[2] = 1.0f / w;
    vs.surface[3] = 1.0f / h;
    vs.flags[0] = R(OUTPUT_MASK);
    vs.flags[1] = R(TWO_SIDE_LIGHT) ? 1 : 0;
    vs.flags[2] = R(POINT_SIZE);
    vs.flags[3] = R(BRANCH_BITS);
    if (!upload_uniform(&vs, sizeof vs, ubo[1])) return skip(__LINE__);
    std::vector<float> lits(std::max<size_t>(1, fp->literal_slots.size()) * 4, 0.0f);
    for (size_t i = 0; i < fp->literal_slots.size(); i++) {
        uint32_t a = fp_ea + (fp->start_slot + fp->literal_slots[i]) * 16;
        for (uint32_t k = 0; k < 4; k++) {
            uint32_t v = vm::read32(a + k * 4);
            v = (v << 16) | (v >> 16);
            std::memcpy(&lits[i * 4 + k], &v, 4);
        }
    }
    if (!upload_uniform(lits.data(), lits.size() * 4, ubo[2])) return skip(__LINE__);
    if (mv::enabled() && fp->hash == mv::kCameraVelocityFp && !colors.empty()) mv::set_camera(lits.data(), lits.size() / 4, colors[0]->offset, texture_regs(1).offset);
    struct FragmentState {
        float window[4];
        float fog[4];
        uint32_t flags[4];
        float texscale[16];
    } fs{};
    for (uint32_t u = 0; u < 16; u++) fs.texscale[u] = tex_scales[u];
    fs.fog[2] = colors.empty() ? (depth ? depth->scale : 1.0f) : colors[0]->scale;
    uint32_t win = R(SHADER_WINDOW);
    uint32_t cfmt = R(SURFACE_FORMAT) & 0x1F;
    uint32_t aref = R(ALPHA_REF);
    float alpha_ref;
    if (cfmt == 0xB) alpha_ref = half_to_float(uint16_t(aref));
    else if (cfmt == 0xC || cfmt == 0xD) std::memcpy(&alpha_ref, &aref, 4);
    else alpha_ref = (aref & 0xFF) / 255.0f;
    fs.window[0] = float(win & 0xFFF);
    fs.window[1] = ((win >> 12) & 0xF) ? 1.0f : 0.0f;
    fs.window[2] = ((win >> 16) & 0xF) ? 1.0f : 0.0f;
    fs.window[3] = alpha_ref;
    fs.fog[0] = RF(FOG_PARAMS);
    fs.fog[1] = RF(FOG_PARAMS + 4);
    bool srgb_out = R(SHADER_PACKER) && !(R(SHADER_CONTROL) & 0x40) && cfmt != 0xB && cfmt != 0xC && cfmt != 0xD && !(g_experiment & 128);
    fs.flags[0] = (R(ALPHA_TEST_ENABLE) ? 1u : 0u) | (srgb_out ? 2u : 0u);
    fs.flags[1] = R(FOG_MODE);
    fs.flags[2] = R(ALPHA_FUNC) & 7;
    static uint32_t fp_debug = std::getenv("GOWA_FP_DEBUG") ? std::atoi(std::getenv("GOWA_FP_DEBUG")) : 0;
    fs.flags[3] = fp_debug;
    if (!upload_uniform(&fs, sizeof fs, ubo[3])) return skip(__LINE__);
    for (uint32_t i = 0; i < 4; i++) {
        writes[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, i, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &ubo[i], nullptr};
        nw++;
    }

    g_stats.phase[2] += ticks() - ph;
    ph = ticks();
    VkBuffer vbufs[16];
    VkDeviceSize voffs[16];
    VkDeviceSize vstrides[16];
    const float* vdata[16] = {};
    uint32_t div_ops = R(FREQUENCY_DIVIDER);
    uint32_t inline_offset = 0;
    std::vector<uint8_t> stream;
    if (inline_draw) {
        stream.resize(g_inline.size() * 4);
        for (size_t i = 0; i < g_inline.size(); i++) {
            uint32_t v = __builtin_bswap32(g_inline[i]);
            std::memcpy(&stream[i * 4], &v, 4);
        }
    }
    struct PendingDecode {
        uint32_t type, size, stride;
        const uint8_t* src;
        float* out;
    };
    PendingDecode pending[16];
    uint32_t npending = 0;
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t f = R(VERTEX_ARRAY_FORMAT + i * 4);
        uint32_t type = f & 7, size = (f >> 4) & 0xF, stride = (f >> 8) & 0xFF, freq = f >> 16;
        uint32_t host = size ? inline_host_bytes(type, size) : 0;
        uint32_t my_inline = inline_offset;
        if (inline_draw) inline_offset += host;
        if (!(vp->inputs & (1u << i))) continue;
        RingAlloc a;
        bool constant = !size;
        uint32_t base = 0;
        if (!constant && !inline_draw) {
            uint32_t off = R(VERTEX_ARRAY_OFFSET + i * 4);
            base = lv2::rsx_address((R(VERTEX_BASE_OFFSET) + (off & 0x7FFFFFFF)) & 0x0FFFFFFF, off >> 31);
            if (!base) constant = true;
        }
        if (constant) {
            if (!ring_alloc(16, 16, a)) return skip(__LINE__);
            static const float fallback[4] = {0, 0, 0, 1};
            std::memcpy(a.ptr, size ? fallback : g_imm[i], 16);
            vbufs[i] = a.buffer;
            voffs[i] = a.offset;
            vstrides[i] = 0;
            vdata[i] = reinterpret_cast<const float*>(a.ptr);
            continue;
        }
        if (!ring_alloc(uint64_t(vcount) * 16, 16, a)) return skip(__LINE__);
        float* out = reinterpret_cast<float*>(a.ptr);
        if (inline_draw) {
            for (uint32_t v = 0; v < vcount; v++) {
                size_t pos = size_t(vmin + v) * inline_stride + my_inline;
                if (pos + attribute_bytes(type, size) <= stream.size()) decode_attribute(type, size, &stream[pos], out + v * 4);
            }
        } else {
            uint32_t last = stride == 0 ? 0 : freq > 1 ? ((div_ops >> i) & 1 ? freq - 1 : vmax / freq) : vmax;
            if (!readable(base, uint64_t(last) * stride + attribute_bytes(type, size))) return skip(__LINE__);
            if (stride && freq <= 1) {
                pending[npending++] = {type, size, stride, vm::ptr(base + vmin * stride), out};
            } else {
                for (uint32_t v = 0; v < vcount; v++) {
                    uint32_t idx = vmin + v;
                    uint32_t element = idx;
                    if (stride == 0) element = 0;
                    else if (freq > 1) element = (div_ops >> i) & 1 ? idx % freq : idx / freq;
                    decode_attribute(type, size, vm::ptr(base + element * stride), out + v * 4);
                }
            }
            if (g_trace && stride && freq <= 1) {
                uint32_t bytes = (vmax + 1) * stride;
                g_vb_checks.push_back({g_frame_draw, i, base, bytes, vb_hash(base, bytes)});
            }
        }
        vbufs[i] = a.buffer;
        voffs[i] = a.offset;
        vstrides[i] = 16;
        vdata[i] = out;
    }
    for (uint32_t v0 = 0; v0 < vcount; v0 += 256) {
        uint32_t nv = std::min<uint32_t>(256, vcount - v0);
        for (uint32_t k = 0; k < npending; k++) {
            const PendingDecode& d = pending[k];
            decode_vertices(d.type, d.size, d.src + size_t(v0) * d.stride, d.stride, nv, d.out + size_t(v0) * 4);
        }
    }
    g_stats.phase[3] += ticks() - ph;
    ph = ticks();
    RingAlloc ib;
    if (!ring_alloc(indices.size() * 4, 16, ib)) return skip(__LINE__);
    uint32_t* idx_out = reinterpret_cast<uint32_t*>(ib.ptr);
    std::memcpy(idx_out, indices.data(), indices.size() * 4);
    static const char* geom_dir = std::getenv("GOWA_DUMP_GEOM");
    if (g_trace && geom_dir && vdata[0]) {
        char fn[512];
        std::snprintf(fn, sizeof fn, "%s/geom_%04u.bin", geom_dir, g_frame_draw);
        if (FILE* f = std::fopen(fn, "wb")) {
            uint32_t hdr[8] = {0x4D4F4547u, g_frame_draw, g_prim, vcount, static_cast<uint32_t>(indices.size()), static_cast<uint32_t>(vstrides[0]), colors.empty() ? 0u : colors[0]->offset, R(COLOR_MASK)};
            std::fwrite(hdr, 4, 8, f);
            std::fwrite(&g_vp_constants[264 * 4], 4, 16, f);
            std::fwrite(vdata[0], 4, size_t(vstrides[0] ? vcount : 1) * 4, f);
            std::fwrite(idx_out, 4, indices.size(), f);
            std::fclose(f);
        }
    }

    PipelineKey k{};
    k.vp = reinterpret_cast<uint64_t>(vp->module);
    k.fp = reinterpret_cast<uint64_t>(fp->module);
    k.inputs = vp->inputs;
    k.color_count = static_cast<uint32_t>(colors.size());
    for (uint32_t i = 0; i < k.color_count; i++) k.color_formats[i] = colors[i]->format;
    k.depth_format = depth ? depth->format : VK_FORMAT_UNDEFINED;
    k.blend_mask = (R(BLEND_ENABLE) ? 1u : 0u) | (R(BLEND_ENABLE_MRT) & 0xE);
    k.sfactor = R(BLEND_SFACTOR);
    k.dfactor = R(BLEND_DFACTOR);
    k.equation = R(BLEND_EQUATION);
    for (uint32_t i = 0; i < k.color_count; i++) k.write_masks |= color_write_mask(i) << (4 * i);
    k.logic = R(LOGIC_OP_ENABLE) ? (R(LOGIC_OP) | 0x10000) : 0;
    uint32_t pm = R(FRONT_POLYGON_MODE);
    k.polygon_mode = pm == 0x1B00 ? VK_POLYGON_MODE_POINT : pm == 0x1B01 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    k.topology_class = topology_class(topo);
    g_stats.phase[4] += ticks() - ph;
    ph = ticks();
    VkPipeline pipe = pipeline(k, vp->module, fp->module);
    if (!pipe) return skip(__LINE__);

    static int draw_log = std::getenv("GOWA_DRAW_LOG") ? std::atoi(std::getenv("GOWA_DRAW_LOG")) : 0;
    static uint64_t draw_every = std::getenv("GOWA_DRAW_LOG_EVERY") ? std::strtoull(std::getenv("GOWA_DRAW_LOG_EVERY"), nullptr, 0) : 1;
    static uint64_t draw_counter = 0;
    if (g_trace || (draw_log > 0 && (draw_counter++ % draw_every) == 0)) {
        if (!g_trace) draw_log--;
        float p0[4] = {};
        if (vstrides[0] == 16 || vstrides[0] == 0) {
            RingAlloc dummy;
            (void)dummy;
        }
        LOG_INFO("rsx", "draw #%u prim %u idx %zu verts %u [%u..%u] targets %zu c0 0x%x fmt %u depth 0x%x %ux%u scissor %u,%u-%u,%u vs %.3f %.3f %.3f off %.3f %.3f %.3f fptex 0x%x inline %d blend 0x%x mask 0x%x omask 0x%x", g_frame_draw, g_prim,
                 indices.size(), vcount, vmin, vmax, colors.size(), colors.empty() ? 0 : colors[0]->offset, R(SURFACE_FORMAT) & 0x1F, depth ? depth->offset : 0, w, h, x0, y0, x1, y1, vs.scale[0],
                 vs.scale[1], vs.scale[2], vs.offset[0], vs.offset[1], vs.offset[2], fp->textures, inline_draw, k.blend_mask, k.write_masks, vs.flags[0]);
        (void)p0;
        TextureRegs t0 = texture_regs(0);
        LOG_INFO("rsx", "  stencil func 0x%x ref 0x%x fmask 0x%x wmask 0x%x ops 0x%x/0x%x/0x%x two %u back func 0x%x ref 0x%x | zclear 0x%x alpha %u/0x%x/0x%x", R(STENCIL_FUNC), R(STENCIL_REF), R(STENCIL_FUNC_MASK), R(STENCIL_MASK),
                 R(STENCIL_OP_FAIL), R(STENCIL_OP_FAIL + 4), R(STENCIL_OP_FAIL + 8), R(TWO_SIDED_STENCIL), R(BACK_STENCIL_FUNC), R(BACK_STENCIL_REF), R(ZSTENCIL_CLEAR_VALUE), R(ALPHA_TEST_ENABLE), R(ALPHA_FUNC), R(ALPHA_REF));
        LOG_INFO("rsx", "  vp hash %016llx c264 %g %g %g %g c265 %g %g %g %g c266 %g %g %g %g c267 %g %g %g %g", static_cast<unsigned long long>(vp->hash), g_vp_constants[1056], g_vp_constants[1057], g_vp_constants[1058],
                 g_vp_constants[1059], g_vp_constants[1060], g_vp_constants[1061], g_vp_constants[1062], g_vp_constants[1063], g_vp_constants[1064], g_vp_constants[1065], g_vp_constants[1066],
                 g_vp_constants[1067], g_vp_constants[1068], g_vp_constants[1069], g_vp_constants[1070], g_vp_constants[1071]);
        LOG_INFO("rsx", "  fp hash %016llx depth test %u func 0x%x mask %u cull %u/0x%x front 0x%x stencil %u", static_cast<unsigned long long>(fp->hash), R(DEPTH_TEST_ENABLE), R(DEPTH_FUNC), R(DEPTH_MASK),
                 R(CULL_FACE_ENABLE), R(CULL_FACE), R(FRONT_FACE), R(STENCIL_TEST_ENABLE));
        LOG_INFO("rsx", "  tex0 off 0x%x fmt 0x%x addr 0x%x c0 0x%x c1 0x%x filt 0x%x rect 0x%x c3 0x%x | sf 0x%x df 0x%x eq 0x%x | shctl 0x%x fp 0x%x", t0.offset, t0.format, t0.address, t0.control0,
                 t0.control1, t0.filter, t0.rect, t0.control3, k.sfactor, k.dfactor, k.equation, R(SHADER_CONTROL), R(SHADER_PROGRAM));
        for (uint32_t i = 0; i < 16; i++) {
            if (!(vp->inputs & (1u << i))) continue;
            uint32_t f = R(VERTEX_ARRAY_FORMAT + i * 4);
            const float* d = vdata[i];
            LOG_INFO("rsx", "  input %u fmt 0x%x off 0x%x stride %u idxbuf 0x%x v0 %g %g %g %g", i, f, R(VERTEX_ARRAY_OFFSET + i * 4), unsigned(vstrides[i]), lv2::rsx_address(R(INDEX_ARRAY_ADDRESS), R(INDEX_ARRAY_DMA) & 0xF), d ? d[0] : 0, d ? d[1] : 0, d ? d[2] : 0, d ? d[3] : 0);
        }
        if (vdata[0] && vstrides[0] == 16 && indices.size() >= 3) {
            const float* m = &g_vp_constants[264 * 4];
            float wx[3], wy[3], ww[3];
            for (int k = 0; k < 3; k++) {
                const float* v = vdata[0] + (indices[k] - vmin) * 4;
                float cx = v[0] * m[0] + v[1] * m[4] + v[2] * m[8] + v[3] * m[12];
                float cy = v[0] * m[1] + v[1] * m[5] + v[2] * m[9] + v[3] * m[13];
                float cw = v[0] * m[3] + v[1] * m[7] + v[2] * m[11] + v[3] * m[15];
                ww[k] = cw;
                wx[k] = cx / cw * vs.scale[0] + vs.offset[0];
                wy[k] = cy / cw * vs.scale[1] + vs.offset[1];
            }
            float area = (wx[1] - wx[0]) * (wy[2] - wy[0]) - (wx[2] - wx[0]) * (wy[1] - wy[0]);
            LOG_INFO("rsx", "  tri0 win (%.0f,%.0f) (%.0f,%.0f) (%.0f,%.0f) w %.2f %.2f %.2f area %.0f", wx[0], wy[0], wx[1], wy[1], wx[2], wy[2], ww[0], ww[1], ww[2], area);
        }
        LOG_INFO("rsx", "  vp start %u c0 %g %g %g %g c1 %g %g %g %g c2 %g %g %g %g c3 %g %g %g %g", R(TRANSFORM_PROGRAM_START), g_vp_constants[0], g_vp_constants[1], g_vp_constants[2], g_vp_constants[3], g_vp_constants[4],
                 g_vp_constants[5], g_vp_constants[6], g_vp_constants[7], g_vp_constants[8], g_vp_constants[9], g_vp_constants[10], g_vp_constants[11], g_vp_constants[12], g_vp_constants[13], g_vp_constants[14],
                 g_vp_constants[15]);
        if (inline_draw) {
            std::string words;
            for (size_t i = 0; i < std::min<size_t>(g_inline.size(), 24); i++) {
                char b[16];
                std::snprintf(b, sizeof b, "%08x ", g_inline[i]);
                words += b;
            }
            LOG_INFO("rsx", "  inline stride %u: %s", inline_stride, words.c_str());
        }
        for (size_t i = 0; i < lits.size() / 4; i++) LOG_INFO("rsx", "  fc[%zu] = %f %f %f %f", i, lits[i * 4], lits[i * 4 + 1], lits[i * 4 + 2], lits[i * 4 + 3]);
        uint32_t tea = lv2::rsx_address(t0.offset, (t0.format & 3) - 1);
        if (tea && readable(tea, 64)) {
            const uint8_t* tp = vm::ptr(tea + 0x1400 * 360 + 640 * 4);
            LOG_INFO("rsx", "  tex0 ea 0x%08x center %02x%02x%02x%02x %02x%02x%02x%02x", tea, tp[0], tp[1], tp[2], tp[3], tp[4], tp[5], tp[6], tp[7]);
        }
    }
    g_stats.phase[5] += ticks() - ph;
    ph = ticks();
    static const bool zpass_real = std::getenv("GOWA_ZPASS_REAL") != nullptr;
    bool zcount = zpass_real && R(0x17CC) != 0;
    if (zcount && g_qreset_needed) {
        if (!g_qpool) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_OCCLUSION;
            qi.queryCount = QMAX;
            vkCreateQueryPool(vk().device, &qi, nullptr, &g_qpool);
        }
        if (g_qpool) {
            end_pass();
            vkCmdResetQueryPool(frame_cmd(), g_qpool, 0, QMAX);
            g_qused = 0;
            g_qreset_needed = false;
        }
    }
    if (mv::enabled()) mv_draw(colors, w, h);
    begin_pass(colors, depth, w, h);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeline_layout, 0, nw, writes);
    for (uint32_t i = 0; i < 16; i++) {
        if (vp->inputs & (1u << i)) vkCmdBindVertexBuffers2(cmd, i, 1, &vbufs[i], &voffs[i], nullptr, &vstrides[i]);
    }
    vkCmdBindIndexBuffer(cmd, ib.buffer, ib.offset, VK_INDEX_TYPE_UINT32);
    float ps = colors.empty() ? (depth ? depth->scale : 1.0f) : colors[0]->scale;
    VkViewport viewport{0, 0, float(w) * ps, float(h) * ps, 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    uint32_t scx0 = x0 ? up(x0, ps) : 0, scy0 = y0 ? up(y0, ps) : 0;
    VkRect2D scissor{{int32_t(scx0), int32_t(scy0)}, {up(x1, ps) - scx0, up(y1, ps) - scy0}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    uint32_t lw = R(LINE_WIDTH);
    vkCmdSetLineWidth(cmd, vk().features.wideLines ? std::max(1.0f, (lw >> 3) + (lw & 7) / 8.0f) : 1.0f);
    vkCmdSetDepthBiasEnable(cmd, R(POLY_OFFSET_FILL) ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthBias(cmd, RF(POLY_OFFSET_BIAS), 0.0f, RF(POLY_OFFSET_SCALE));
    uint32_t bc = R(BLEND_COLOR);
    float blend[4] = {((bc >> 16) & 0xFF) / 255.0f, ((bc >> 8) & 0xFF) / 255.0f, (bc & 0xFF) / 255.0f, ((bc >> 24) & 0xFF) / 255.0f};
    vkCmdSetBlendConstants(cmd, blend);
    bool two_sided = R(TWO_SIDED_STENCIL) != 0;
    uint32_t fb[2][4] = {{STENCIL_MASK, STENCIL_FUNC, STENCIL_REF, STENCIL_FUNC_MASK}, {BACK_STENCIL_MASK, BACK_STENCIL_FUNC, BACK_STENCIL_REF, BACK_STENCIL_FUNC_MASK}};
    uint32_t ops[2] = {STENCIL_OP_FAIL, BACK_STENCIL_OP_FAIL};
    for (uint32_t face = 0; face < 2; face++) {
        uint32_t s = two_sided ? face : 0;
        VkStencilFaceFlags ff = face ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
        vkCmdSetStencilWriteMask(cmd, ff, R(fb[s][0]) & 0xFF);
        vkCmdSetStencilReference(cmd, ff, R(fb[s][2]) & 0xFF);
        vkCmdSetStencilCompareMask(cmd, ff, R(fb[s][3]) & 0xFF);
        vkCmdSetStencilOp(cmd, ff, stencil_op(R(ops[s])), stencil_op(R(ops[s] + 8)), stencil_op(R(ops[s] + 4)), compare_op(R(fb[s][1])));
    }
    bool has_stencil = depth && depth->format == VK_FORMAT_D24_UNORM_S8_UINT;
    vkCmdSetStencilTestEnable(cmd, has_stencil && R(STENCIL_TEST_ENABLE) ? VK_TRUE : VK_FALSE);
    bool dtest = depth && R(DEPTH_TEST_ENABLE);
    vkCmdSetDepthTestEnable(cmd, dtest ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthWriteEnable(cmd, dtest && R(DEPTH_MASK) ? VK_TRUE : VK_FALSE);
    VkCompareOp depth_op = compare_op(R(DEPTH_FUNC));
    if (depth_op == VK_COMPARE_OP_EQUAL && !R(DEPTH_MASK)) depth_op = (g_experiment & 64) ? VK_COMPARE_OP_ALWAYS : VK_COMPARE_OP_LESS_OR_EQUAL;
    vkCmdSetDepthCompareOp(cmd, depth_op);
    if (vk().features.depthBounds) {
        vkCmdSetDepthBoundsTestEnable(cmd, depth && R(DEPTH_BOUNDS_ENABLE) ? VK_TRUE : VK_FALSE);
        vkCmdSetDepthBounds(cmd, std::clamp(RF(DEPTH_BOUNDS_MIN), 0.0f, 1.0f), std::clamp(RF(DEPTH_BOUNDS_MAX), 0.0f, 1.0f));
    }
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    if (R(CULL_FACE_ENABLE)) {
        uint32_t cf = R(CULL_FACE);
        cull = cf == 0x404 ? VK_CULL_MODE_FRONT_BIT : cf == 0x408 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_BACK_BIT;
    }
    if (g_experiment & 16) cull = VK_CULL_MODE_NONE;
    vkCmdSetCullMode(cmd, cull);
    bool cw = R(FRONT_FACE) == 0x900;
    if (g_experiment & 32) cw = !cw;
    vkCmdSetFrontFace(cmd, cw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cmd, topo);
    vkCmdSetPrimitiveRestartEnable(cmd, restart ? VK_TRUE : VK_FALSE);
    bool query = zcount && g_qpool && !g_qreset_needed && g_qused < QMAX;
    uint32_t qidx = 0;
    if (query) {
        qidx = g_qused++;
        vkCmdBeginQuery(cmd, g_qpool, qidx, vk().features.occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    }
    uint32_t draw_index = g_frame_draw++;
    static const bool skip_color_only = std::getenv("GOWA_SKIP_COLOR_ONLY") != nullptr;
    bool skip_draw = g_skip_init && draw_index >= g_skip_lo && draw_index <= g_skip_hi && (!skip_color_only || k.write_masks != 0);
    if (!(g_experiment & 1) && !skip_draw) vkCmdDrawIndexed(cmd, static_cast<uint32_t>(indices.size()), 1, 0, -static_cast<int32_t>(vmin), 0);
    if (query) {
        vkCmdEndQuery(cmd, g_qpool, qidx);
        g_counter_queries.push_back(qidx);
    }
    g_stats.phase[6] += ticks() - ph;
    for (uint32_t i = 0; i < g_pass.color_count; i++) g_pass.colors[i]->generation++;
    if (g_pass.depth) g_pass.depth->generation++;
    g_stats.draws++;
    g_stats.vertices += vcount;
    if (g_trace) {
        static const uint32_t dump_at = std::getenv("GOWA_TRACE_DUMP_DRAW") ? std::atoi(std::getenv("GOWA_TRACE_DUMP_DRAW")) : 0;
        static const uint32_t dump_target = std::getenv("GOWA_TRACE_DUMP_TARGET") ? static_cast<uint32_t>(std::strtoul(std::getenv("GOWA_TRACE_DUMP_TARGET"), nullptr, 16)) : ~0u;
        static bool dumped_target = false;
        static const uint32_t after_target = std::getenv("GOWA_TRACE_DUMP_AFTER_TARGET") ? static_cast<uint32_t>(std::strtoul(std::getenv("GOWA_TRACE_DUMP_AFTER_TARGET"), nullptr, 16)) : ~0u;
        static bool seen_after = false;
        bool on_after = !colors.empty() && colors[0] && colors[0]->offset == after_target;
        static const uint32_t dump_target_count = [] {
            const char* e = std::getenv("GOWA_TRACE_DUMP_TARGET");
            const char* c = e ? std::strchr(e, ':') : nullptr;
            return c ? static_cast<uint32_t>(std::atoi(c + 1)) : 0u;
        }();
        bool hit_target = !dumped_target && !colors.empty() && colors[0] && colors[0]->offset == dump_target && (!dump_target_count || colors.size() == dump_target_count);
        if (!dumped_target && seen_after && !on_after) hit_target = true;
        if (on_after) seen_after = true;
        if (hit_target) dumped_target = true;
        if (++g_trace_draws == dump_at || hit_target) {
            end_pass();
            LOG_INFO("rsx", "dumping surfaces after traced draw %u", g_trace_draws);
            schedule_dumps();
        }
    }
    return true;
}

uint32_t morton(uint32_t x, uint32_t y, uint32_t log_w, uint32_t log_h) {
    uint32_t m = std::min(log_w, log_h), v = 0;
    for (uint32_t k = 0; k < m; k++) v |= (((x >> k) & 1) << (2 * k)) | (((y >> k) & 1) << (2 * k + 1));
    if (log_w > m) v |= (x >> m) << (2 * m);
    else if (log_h > m) v |= (y >> m) << (2 * m);
    return v;
}

uint16_t argb_to_565(uint32_t c) {
    uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    return static_cast<uint16_t>(((r * 32 / 255) << 11) | ((g * 64 / 255) << 5) | (b * 32 / 255));
}

void nv308a_color(uint32_t index, uint32_t arg) {
    if (index >= (R(0xA308) & 0xFFFF)) return;
    uint32_t point = R(0xA304);
    uint32_t x = (point & 0xFFFF) + index, y = point >> 16;
    uint32_t fmt = R(0x6300);
    uint32_t pitch = R(0x6304) >> 16;
    uint32_t bpp = fmt == 4 ? 2 : 4;
    uint32_t ea = lv2::rsx_address(R(0x630C) + x * bpp + y * pitch, R(0x6188));
    if (!ea || !readable(ea, bpp)) return;
    if (trace::on(trace::RSX)) trace::emit(trace::T_COPY, trace::COPY_NV308A, ea, ea, bpp, 0, arg);
    if (bpp == 4) vm::write32(ea, arg);
    else vm::write16(ea, argb_to_565(arg));
}

void nv0039_transfer() {
    if (std::getenv("GOWA_COPY_LOG")) LOG_INFO("rsxcopy", "nv0039 in 0x%x dma 0x%x out 0x%x dma 0x%x pitch %d/%d len %u lines %u fmt 0x%x", R(0x230C), R(0x2184), R(0x2310), R(0x2188), int32_t(R(0x2314)),
                                                 int32_t(R(0x2318)), R(0x231C), R(0x2320), R(0x2324));
    if (g_trace) LOG_INFO("rsx", "trace nv0039 in 0x%x dma 0x%x out 0x%x dma 0x%x pitch %d/%d len %u lines %u fmt 0x%x", R(0x230C), R(0x2184), R(0x2310), R(0x2188), int32_t(R(0x2314)), int32_t(R(0x2318)), R(0x231C), R(0x2320), R(0x2324));
    uint32_t len = R(0x231C), lines = R(0x2320), fmt = R(0x2324);
    uint32_t in_stride = fmt & 0xFF, out_stride = (fmt >> 8) & 0xFF;
    if (!len || !lines || in_stride < 1 || in_stride > 4 || out_stride < 1 || out_stride > 4) return;
    int32_t in_pitch = static_cast<int32_t>(R(0x2314)), out_pitch = static_cast<int32_t>(R(0x2318));
    uint32_t src = lv2::rsx_address(R(0x230C), R(0x2184));
    uint32_t dst = lv2::rsx_address(R(0x2310), R(0x2188));
    if (!src || !dst) return;
    if (trace::on(trace::RSX)) trace::emit(trace::T_COPY, trace::COPY_NV0039 | (in_stride << 8) | (out_stride << 16), src, dst, len, lines, uint64_t(uint32_t(in_pitch)) | (uint64_t(uint32_t(out_pitch)) << 32));
    static std::vector<uint8_t> line;
    line.resize(len);
    for (uint32_t l = 0; l < lines; l++) {
        uint32_t s = src + static_cast<uint32_t>(int64_t(in_pitch) * l), d = dst + static_cast<uint32_t>(int64_t(out_pitch) * l);
        if (!readable(s, uint64_t(len - 1) * in_stride + 1) || !readable(d, uint64_t(len - 1) * out_stride + 1)) return;
        vm::host_write(d, uint64_t(len - 1) * out_stride + 1);
        if (in_stride == 1 && out_stride == 1) {
            std::memmove(vm::ptr(d), vm::ptr(s), len);
            continue;
        }
        for (uint32_t i = 0; i < len; i++) line[i] = *vm::ptr(s + i * in_stride);
        for (uint32_t i = 0; i < len; i++) *vm::ptr(d + i * out_stride) = line[i];
    }
}

void nv3089_transfer(uint32_t arg) {
    if (std::getenv("GOWA_COPY_LOG")) LOG_INFO("rsxcopy", "nv3089 src 0x%x dma 0x%x in 0x%x fmt 0x%x out 0x%x/0x%x clip 0x%x/0x%x dst 0x%x dma 0x%x pitch 0x%x surf 0x%x cfmt 0x%x/0x%x dsdx 0x%x/0x%x arg 0x%x", R(0xC408), R(0xC184),
                                                 R(0xC400), R(0xC404), R(0xC310), R(0xC314), R(0xC308), R(0xC30C), R(0x630C), R(0x6188), R(0x6304), R(0xC198), R(0xC300), R(0x6300), R(0xC318), R(0xC31C), arg);
    if (g_trace) LOG_INFO("rsx", "trace nv3089 src 0x%x dma 0x%x in 0x%x fmt 0x%x out 0x%x/0x%x size 0x%x dst 0x%x dma 0x%x surf 0x%x dsdx 0x%x", R(0xC408), R(0xC184), R(0xC400), R(0xC404), R(0xC310), R(0xC314), R(0xC30C), R(0x630C), R(0x6188), R(0xC198), R(0xC318));
    if (R(0xC304) != 3) {
        if (std::getenv("GOWA_COPY_LOG")) LOG_WARN("rsxcopy", "nv3089 skipped at line %d", __LINE__);
        return;
    }
    uint32_t in_size = R(0xC400), in_fmt = R(0xC404);
    uint32_t in_w = in_size & 0xFFFF, in_h = in_size >> 16;
    uint32_t src_bpp = R(0xC300) == 7 ? 2 : 4;
    uint32_t in_pitch = in_fmt & 0xFFFF;
    if (!in_pitch) in_pitch = in_w * src_bpp;
    bool center = ((in_fmt >> 16) & 0xFF) == 1;
    float fx = (arg & 0xFFFF) / 16.0f, fy = (arg >> 16) / 16.0f;
    uint32_t in_x = center ? uint32_t(std::floor(std::max(fx, 0.5f) - 0.5f)) : uint32_t(fx);
    uint32_t in_y = center ? uint32_t(std::floor(std::max(fy, 0.5f) - 0.5f)) : uint32_t(fy);
    uint32_t out_point = R(0xC310), out_size = R(0xC314), clip_point = R(0xC308), clip_size = R(0xC30C);
    uint32_t out_x = out_point & 0xFFFF, out_y = out_point >> 16, out_w = out_size & 0xFFFF, out_h = out_size >> 16;
    uint32_t clip_x = clip_point & 0xFFFF, clip_y = clip_point >> 16;
    uint32_t clip_w = std::min(clip_size & 0xFFFF, out_w), clip_h = std::min(clip_size >> 16, out_h);
    if (!clip_w || !clip_h || !in_w || !in_h) {
        if (std::getenv("GOWA_COPY_LOG")) LOG_WARN("rsxcopy", "nv3089 skipped at line %d", __LINE__);
        return;
    }
    int32_t dsdx_raw = static_cast<int32_t>(R(0xC318)), dtdy_raw = static_cast<int32_t>(R(0xC31C));
    if (!dsdx_raw || !dtdy_raw) {
        if (std::getenv("GOWA_COPY_LOG")) LOG_WARN("rsxcopy", "nv3089 skipped at line %d", __LINE__);
        return;
    }
    double dsdx = dsdx_raw / 1048576.0, dtdy = dtdy_raw / 1048576.0;
    uint32_t src = lv2::rsx_address(R(0xC408) + in_y * in_pitch + in_x * src_bpp, R(0xC184));
    if (!src) {
        if (std::getenv("GOWA_COPY_LOG")) LOG_WARN("rsxcopy", "nv3089 skipped at line %d", __LINE__);
        return;
    }
    bool swizzled = R(0xC198) == 0x31337A73;
    uint32_t dst_fmt = swizzled ? (R(0x8300) & 0xFFFF) : R(0x6300);
    uint32_t dst_bpp = dst_fmt == 4 ? 2 : 4;
    uint32_t dst_pitch = R(0x6304) >> 16;
    uint32_t log_w = (R(0x8300) >> 16) & 0xFF, log_h = (R(0x8300) >> 24) & 0xFF;
    uint32_t dst = swizzled ? lv2::rsx_address(R(0x8304), R(0x8184)) : lv2::rsx_address(R(0x630C), R(0x6188));
    if (!dst) {
        if (std::getenv("GOWA_COPY_LOG")) LOG_WARN("rsxcopy", "nv3089 skipped at line %d", __LINE__);
        return;
    }
    uint32_t x0 = std::max(clip_x, out_x), y0 = std::max(clip_y, out_y);
    uint32_t x1 = std::min(clip_x + clip_w, out_x + out_w), y1 = std::min(clip_y + clip_h, out_y + out_h);
    if (x1 <= x0 || y1 <= y0) {
        if (std::getenv("GOWA_COPY_LOG")) LOG_WARN("rsxcopy", "nv3089 skipped at line %d", __LINE__);
        return;
    }
    if (!readable(src, uint64_t(in_h - 1 - std::min(in_y, in_h - 1)) * in_pitch + in_w * src_bpp)) {
        if (std::getenv("GOWA_COPY_LOG")) LOG_WARN("rsxcopy", "nv3089 skipped at line %d", __LINE__);
        return;
    }
    if (trace::on(trace::RSX)) {
        bool scaled = dsdx_raw != (1 << 20) || dtdy_raw != (1 << 20);
        trace::emit(trace::T_COPY, trace::COPY_NV3089, src, dst, uint64_t(x0) | (uint64_t(y0) << 16) | (uint64_t(x1) << 32) | (uint64_t(y1) << 48),
                    uint64_t(in_pitch) | (uint64_t(swizzled ? 0 : dst_pitch) << 32),
                    uint64_t(out_x) | (uint64_t(out_y) << 16) | (uint64_t(src_bpp) << 32) | (uint64_t(dst_bpp) << 40) | (uint64_t(swizzled) << 48) | (uint64_t(scaled) << 49));
    }
    bool unscaled = dsdx_raw == (1 << 20) && dtdy_raw == (1 << 20);
    if (unscaled && !swizzled && src_bpp == dst_bpp && x1 - out_x <= in_w && y1 - out_y <= in_h) {
        uint32_t row = (x1 - x0) * dst_bpp;
        uint64_t s_lo = src + uint64_t(y0 - out_y) * in_pitch + (x0 - out_x) * src_bpp, s_hi = src + uint64_t(y1 - 1 - out_y) * in_pitch + (x0 - out_x) * src_bpp + row;
        uint64_t d_lo = dst + uint64_t(y0) * dst_pitch + x0 * dst_bpp, d_hi = dst + uint64_t(y1 - 1) * dst_pitch + x0 * dst_bpp + row;
        if (d_hi > 0x100000000ull || !vm::range_mapped(uint32_t(d_lo), d_hi - d_lo)) {
            if (std::getenv("GOWA_COPY_LOG")) LOG_WARN("rsxcopy", "nv3089 skipped at line %d", __LINE__);
            return;
        }
        vm::host_write(uint32_t(d_lo), d_hi - d_lo);
        bool overlap = s_lo < d_hi && d_lo < s_hi;
        static std::vector<uint8_t> staging;
        if (overlap) {
            staging.resize(size_t(row) * (y1 - y0));
            for (uint32_t y = y0; y < y1; y++) std::memcpy(&staging[size_t(y - y0) * row], vm::ptr(src + (y - out_y) * in_pitch + (x0 - out_x) * src_bpp), row);
            for (uint32_t y = y0; y < y1; y++) std::memcpy(vm::ptr(dst + y * dst_pitch + x0 * dst_bpp), &staging[size_t(y - y0) * row], row);
        } else {
            for (uint32_t y = y0; y < y1; y++) std::memcpy(vm::ptr(dst + y * dst_pitch + x0 * dst_bpp), vm::ptr(src + (y - out_y) * in_pitch + (x0 - out_x) * src_bpp), row);
        }
        g_stats.blit_fast++;
        return;
    }
    g_stats.blit_slow++;
    static std::vector<int32_t> cols;
    cols.resize(x1 - x0);
    for (uint32_t x = x0; x < x1; x++) {
        double su = (x - out_x + 0.5) * dsdx;
        cols[x - x0] = std::clamp(int32_t(su < 0 ? in_w + std::floor(su) : std::floor(su)), 0, int32_t(in_w - 1)) * int32_t(src_bpp);
    }
    static std::vector<uint32_t> pixels;
    pixels.resize(size_t(x1 - x0) * (y1 - y0));
    size_t k = 0;
    for (uint32_t y = y0; y < y1; y++) {
        double sv = (y - out_y + 0.5) * dtdy;
        int32_t sy = std::clamp(int32_t(sv < 0 ? in_h + std::floor(sv) : std::floor(sv)), 0, int32_t(in_h - 1));
        const uint8_t* rowp = vm::ptr(src + sy * in_pitch);
        if (src_bpp == 4) {
            for (uint32_t x = x0; x < x1; x++) {
                uint32_t v;
                std::memcpy(&v, rowp + cols[x - x0], 4);
                pixels[k++] = __builtin_bswap32(v);
            }
        } else {
            for (uint32_t x = x0; x < x1; x++) {
                uint16_t v;
                std::memcpy(&v, rowp + cols[x - x0], 2);
                pixels[k++] = __builtin_bswap16(v);
            }
        }
    }
    if (swizzled) vm::host_write(dst, (uint64_t(1) << log_w) * (uint64_t(1) << log_h) * dst_bpp);
    else vm::host_write(dst + y0 * dst_pitch, uint64_t(y1 - y0) * dst_pitch);
    k = 0;
    for (uint32_t y = y0; y < y1; y++) {
        for (uint32_t x = x0; x < x1; x++) {
            uint32_t pixel = pixels[k++];
            uint32_t d = swizzled ? dst + morton(x, y, log_w, log_h) * dst_bpp : dst + y * dst_pitch + x * dst_bpp;
            if (!vm::is_mapped(d)) return;
            if (dst_bpp == 4) vm::write32(d, src_bpp == 4 ? pixel : 0xFF000000u | (((pixel >> 11) & 0x1F) * 255 / 31) << 16 | (((pixel >> 5) & 0x3F) * 255 / 63) << 8 | ((pixel & 0x1F) * 255 / 31));
            else vm::write16(d, src_bpp == 2 ? uint16_t(pixel) : argb_to_565(pixel));
        }
    }
}

bool copy_engine(uint32_t reg, uint32_t arg) {
    if (reg >= 0xA400 && reg < 0xC000) {
        nv308a_color((reg - 0xA400) / 4, arg);
        return true;
    }
    if (reg == 0x2328) {
        nv0039_transfer();
        return true;
    }
    if (reg == 0xC40C) {
        nv3089_transfer(arg);
        return true;
    }
    return false;
}

struct Writeback {
    uint8_t* data;
    uint32_t ea;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
};

std::vector<Writeback> g_writebacks;

uint32_t format_bytes(VkFormat f);

struct Dump {
    uint8_t* data;
    uint32_t offset;
    uint32_t width;
    uint32_t height;
    VkFormat format;
    bool main;
    std::string name;
};

std::vector<Dump> g_dumps;

void schedule_dumps() {
    static const std::string only = std::getenv("GOWA_DUMP_ONLY") ? std::string(",") + std::getenv("GOWA_DUMP_ONLY") + "," : std::string();
    for (auto& [k, s] : g_surfaces) {
        if (!only.empty()) {
            char key[16];
            std::snprintf(key, sizeof key, ",%x,", s->offset);
            if (only.find(key) == std::string::npos) continue;
        }
        if (s->depth() && s->format != VK_FORMAT_D24_UNORM_S8_UINT) continue;
        uint32_t bpp = s->depth() ? 4 : format_bytes(s->format);
        if (bpp != 4) continue;
        RingAlloc a;
        if (!ring_alloc(uint64_t(s->iw) * s->ih * 4, 16, a)) continue;
        transition(s.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c{};
        c.bufferOffset = a.offset;
        c.imageSubresource = {VkImageAspectFlags(s->depth() ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, 1};
        c.imageExtent = {s->iw, s->ih, 1};
        vkCmdCopyImageToBuffer(frame_cmd(), s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, a.buffer, 1, &c);
        g_dumps.push_back({a.ptr, s->offset, s->iw, s->ih, s->format, s->main});
    }
}

void schedule_snapshot(Surface* s, uint32_t w, uint32_t h, const std::string& name) {
    if (s->depth() || format_bytes(s->format) != 4) return;
    w = std::min(up(w, s->scale), s->iw);
    h = std::min(up(h, s->scale), s->ih);
    RingAlloc a;
    if (!ring_alloc(uint64_t(w) * h * 4, 16, a)) return;
    transition(s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c{};
    c.bufferOffset = a.offset;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(frame_cmd(), s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, a.buffer, 1, &c);
    g_dumps.push_back({a.ptr, s->offset, w, h, s->format, s->main, name});
}

void complete_dumps() {
    for (const Dump& d : g_dumps) {
        char fn[512];
        if (!d.name.empty()) std::snprintf(fn, sizeof fn, "%s", d.name.c_str());
        else std::snprintf(fn, sizeof fn, "surface_%s_%08x_%ux%u_f%d.ppm", d.main ? "main" : (d.format == VK_FORMAT_D24_UNORM_S8_UINT ? "depth" : "local"), d.offset, d.width, d.height, int(d.format));
        FILE* f = std::fopen(fn, "wb");
        if (!f) continue;
        std::fprintf(f, "P6\n%u %u\n255\n", d.width, d.height);
        std::vector<uint8_t> row(d.width * 3);
        float zmin = 1e30f, zmax = -1e30f;
        for (uint32_t y = 0; y < d.height; y++) {
            const uint8_t* p = d.data + uint64_t(y) * d.width * 4;
            for (uint32_t x = 0; x < d.width; x++) {
                if (d.format == VK_FORMAT_D24_UNORM_S8_UINT) {
                    uint32_t v;
                    std::memcpy(&v, p + x * 4, 4);
                    float z = (v & 0xFFFFFF) / 16777215.0f;
                    zmin = std::min(zmin, z);
                    zmax = std::max(zmax, z);
                    uint8_t g = uint8_t(std::clamp((1.0f - z) * 20.0f, 0.0f, 1.0f) * 255.0f);
                    row[x * 3] = row[x * 3 + 1] = row[x * 3 + 2] = g;
                } else if (d.format == VK_FORMAT_R32_SFLOAT) {
                    float v;
                    std::memcpy(&v, p + x * 4, 4);
                    zmin = std::min(zmin, v);
                    zmax = std::max(zmax, v);
                    uint8_t g = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f);
                    row[x * 3] = row[x * 3 + 1] = row[x * 3 + 2] = g;
                } else if (d.format == VK_FORMAT_R8G8B8A8_UNORM) {
                    row[x * 3] = p[x * 4];
                    row[x * 3 + 1] = p[x * 4 + 1];
                    row[x * 3 + 2] = p[x * 4 + 2];
                } else {
                    row[x * 3] = p[x * 4 + 2];
                    row[x * 3 + 1] = p[x * 4 + 1];
                    row[x * 3 + 2] = p[x * 4];
                }
            }
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
        if (std::getenv("GOWA_DUMP_ALPHA") && (d.format == VK_FORMAT_B8G8R8A8_UNORM || d.format == VK_FORMAT_R8G8B8A8_UNORM)) {
            std::string an = std::string(fn).substr(0, std::strlen(fn) - 4) + "_alpha.ppm";
            if (FILE* af = std::fopen(an.c_str(), "wb")) {
                std::fprintf(af, "P6\n%u %u\n255\n", d.width, d.height);
                for (uint32_t y = 0; y < d.height; y++) {
                    const uint8_t* p = d.data + uint64_t(y) * d.width * 4;
                    for (uint32_t x = 0; x < d.width; x++) row[x * 3] = row[x * 3 + 1] = row[x * 3 + 2] = p[x * 4 + 3];
                    std::fwrite(row.data(), 1, row.size(), af);
                }
                std::fclose(af);
            }
        }
        if (d.name.empty()) LOG_INFO("rsx", "dumped %s z %f..%f", fn, zmin, zmax);
    }
    g_dumps.clear();
}

uint32_t format_bytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: return 1;
    case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_A1R5G5B5_UNORM_PACK16: case VK_FORMAT_R8G8_UNORM: return 2;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 4;
    }
}

void schedule_writebacks() {
    for (auto& [k, s] : g_surfaces) {
        if (!s->main || !s->dirty || s->depth()) continue;
        s->dirty = false;
        uint32_t ea = lv2::rsx_address(s->offset, 1);
        uint32_t bpp = format_bytes(s->format);
        uint32_t pitch = s->pitch ? s->pitch : s->width * bpp;
        if (!ea || !readable(ea, uint64_t(pitch) * (s->height - 1) + uint64_t(s->width) * bpp)) continue;
        RingAlloc a;
        if (!ring_alloc(uint64_t(s->width) * s->height * bpp, 16, a)) continue;
        transition(s.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c{};
        c.bufferOffset = a.offset;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {s->width, s->height, 1};
        vkCmdCopyImageToBuffer(frame_cmd(), s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, a.buffer, 1, &c);
        g_writebacks.push_back({a.ptr, ea, pitch, s->width, s->height, bpp});
    }
}

void complete_writebacks() {
    for (const Writeback& w : g_writebacks) {
        for (uint32_t y = 0; y < w.height; y++) {
            const uint8_t* src = w.data + uint64_t(y) * w.width * w.bpp;
            uint8_t* dst = vm::ptr(w.ea + y * w.pitch);
            if (w.bpp == 4) {
                for (uint32_t x = 0; x < w.width; x++) {
                    uint32_t v;
                    std::memcpy(&v, src + x * 4, 4);
                    v = __builtin_bswap32(v);
                    std::memcpy(dst + x * 4, &v, 4);
                }
            } else if (w.bpp == 2) {
                for (uint32_t x = 0; x < w.width; x++) {
                    dst[x * 2] = src[x * 2 + 1];
                    dst[x * 2 + 1] = src[x * 2];
                }
            } else if (w.bpp == 8) {
                for (uint32_t x = 0; x < w.width * 4; x++) {
                    dst[x * 2] = src[x * 2 + 1];
                    dst[x * 2 + 1] = src[x * 2];
                }
            } else if (w.bpp == 16) {
                for (uint32_t x = 0; x < w.width * 4; x++) {
                    uint32_t v;
                    std::memcpy(&v, src + x * 4, 4);
                    v = __builtin_bswap32(v);
                    std::memcpy(dst + x * 4, &v, 4);
                }
            } else {
                std::memcpy(dst, src, w.width);
            }
        }
    }
    g_writebacks.clear();
}

void set_immediate(uint32_t attr, const float* v, uint32_t n) {
    if (attr >= 16) return;
    for (uint32_t i = 0; i < 4; i++) g_imm[attr][i] = i < n ? v[i] : (i == 3 ? 1.0f : 0.0f);
}

bool immediate_method(uint32_t reg, uint32_t arg) {
    float f;
    std::memcpy(&f, &arg, 4);
    if (reg >= 0x1C00 && reg < 0x1D00) {
        uint32_t a = (reg - 0x1C00) / 16, c = ((reg - 0x1C00) / 4) & 3;
        if (c == 0) set_immediate(a, g_imm[a], 4);
        g_imm[a][c] = f;
        return true;
    }
    if (reg >= 0x1500 && reg < 0x1600) {
        uint32_t a = (reg - 0x1500) / 16, c = ((reg - 0x1500) / 4) & 3;
        if (c < 3) {
            if (c == 0) g_imm[a][3] = 1.0f;
            g_imm[a][c] = f;
        }
        return true;
    }
    if (reg >= 0x1880 && reg < 0x1900) {
        uint32_t a = (reg - 0x1880) / 8, c = ((reg - 0x1880) / 4) & 1;
        if (c == 0) {
            g_imm[a][2] = 0.0f;
            g_imm[a][3] = 1.0f;
        }
        g_imm[a][c] = f;
        return true;
    }
    if (reg >= 0x1E40 && reg < 0x1E80) {
        float v[1] = {f};
        set_immediate((reg - 0x1E40) / 4, v, 1);
        return true;
    }
    if (reg >= 0x1940 && reg < 0x1980) {
        float v[4] = {(arg & 0xFF) / 255.0f, ((arg >> 8) & 0xFF) / 255.0f, ((arg >> 16) & 0xFF) / 255.0f, (arg >> 24) / 255.0f};
        set_immediate((reg - 0x1940) / 4, v, 4);
        return true;
    }
    if (reg >= 0x1900 && reg < 0x1940) {
        float v[2] = {float(int16_t(arg & 0xFFFF)), float(int16_t(arg >> 16))};
        set_immediate((reg - 0x1900) / 4, v, 2);
        return true;
    }
    if ((reg >= 0x1980 && reg < 0x1A00) || (reg >= 0x0A80 && reg < 0x0B00)) {
        bool scaled = reg < 0x1000;
        uint32_t b = scaled ? 0x0A80 : 0x1980;
        uint32_t a = (reg - b) / 8, c = ((reg - b) / 4) & 1;
        float x = float(int16_t(arg & 0xFFFF)), y = float(int16_t(arg >> 16));
        if (scaled) {
            x = (x + 0.5f) / 32767.5f;
            y = (y + 0.5f) / 32767.5f;
        }
        g_imm[a][c * 2] = x;
        g_imm[a][c * 2 + 1] = y;
        return true;
    }
    return false;
}

}

uint32_t reg(uint32_t method) { return R(method); }

std::atomic<uint32_t> g_zpass_draws{0};

void resolve_zpass() {
    if (g_qpool && g_qused) {
        std::vector<uint32_t> res(g_qused, 0);
        VkResult r = vkGetQueryPoolResults(vk().device, g_qpool, 0, g_qused, res.size() * 4, res.data(), 4, 0);
        static uint32_t frames = 0, reports = 0, zeros = 0, notready = 0, empty = 0;
        frames++;
        if (r == VK_NOT_READY) notready++;
        for (const PendingReport& p : g_pending_reports) {
            reports++;
            if (p.queries.empty()) empty++;
            uint64_t sum = 0;
            for (uint32_t q : p.queries) if (q < res.size()) sum += res[q];
            if (!sum) zeros++;
        }
        if (frames % 120 == 0) {
            LOG_INFO("rsx", "zpass: %u reports, %u zero, %u without queries, %u not-ready frames, %u queries last frame", reports, zeros, empty, notready, g_qused);
            reports = zeros = notready = empty = 0;
        }
        if (r == VK_SUCCESS || r == VK_NOT_READY) {
            for (const PendingReport& p : g_pending_reports) {
                uint64_t sum = 0;
                for (uint32_t q : p.queries) if (q < res.size()) sum += res[q];
                uint32_t v = static_cast<uint32_t>(std::min<uint64_t>(sum, 0xFFFFFFFFull));
                g_zpass_cache[p.ea] = v;
                if (vm::is_mapped(p.ea + 8)) vm::write32(p.ea + 8, v);
            }
        }
    }
    g_pending_reports.clear();
    g_qreset_needed = true;
}

uint32_t zpass_value(uint32_t ea) {
    static const uint32_t big = std::getenv("GOWA_ZPASS_VALUE") ? static_cast<uint32_t>(std::strtoul(std::getenv("GOWA_ZPASS_VALUE"), nullptr, 0)) : 0x100000u;
    if (!g_zpass_draws.load(std::memory_order_relaxed)) return 0u;
    if (!g_qpool) return big;
    g_pending_reports.push_back({ea, g_counter_queries});
    auto it = g_zpass_cache.find(ea);
    return it != g_zpass_cache.end() ? it->second : big;
}

void frame_complete() {
    g_frame_draw = 0;
    double compile_ms = std::chrono::duration<double, std::milli>(g_compile_frame.time).count();
    if (compile_ms > 4.0) {
        LOG_INFO("rsx", "compile stall %.1f ms (vp %u, fp %u, pipelines %u)", compile_ms, g_compile_frame.vp, g_compile_frame.fp, g_compile_frame.pipelines);
    }
    g_compile_frame = {};
    if (!g_vb_checks.empty()) {
        uint32_t changed = 0;
        for (const VbCheck& c : g_vb_checks) {
            if (vm::is_mapped(c.ea) && vb_hash(c.ea, c.size) != c.hash) {
                if (changed++ < 40) LOG_WARN("rsx", "vertex data of draw #%u attr %u at 0x%08x+0x%x changed after the draw", c.draw, c.attr, c.ea, c.size);
            }
        }
        LOG_INFO("rsx", "vertex recheck: %u of %zu arrays changed after their draw", changed, g_vb_checks.size());
        g_vb_checks.clear();
    }
    resolve_zpass();
    complete_writebacks();
    complete_dumps();
    mv::complete();
    if (!std::getenv("GOWA_NO_GC")) collect_garbage();
}

void method(uint32_t reg, uint32_t arg) {
    if (reg >= TRANSFORM_PROGRAM && reg < TRANSFORM_PROGRAM + 128) {
        uint32_t load = R(TRANSFORM_PROGRAM_LOAD);
        if (g_vp_load_word / 4 != load && (g_vp_load_word % 4) == 0) g_vp_load_word = load * 4;
        if (g_vp_load_word < 544 * 4 && g_vp_ucode[g_vp_load_word] != arg) {
            g_vp_ucode[g_vp_load_word] = arg;
            g_vp_ucode_serial++;
        }
        g_vp_load_word++;
        g_regs[(TRANSFORM_PROGRAM_LOAD >> 2)] = g_vp_load_word / 4;
        return;
    }
    if (reg == TRANSFORM_PROGRAM_LOAD) g_vp_load_word = arg * 4;
    if (g_trace && reg == TRANSFORM_CONSTANT_LOAD) LOG_INFO("rsx", "  const load %u", arg);
    if (reg >= TRANSFORM_CONSTANT && reg < TRANSFORM_CONSTANT + 128) {
        uint32_t idx = R(TRANSFORM_CONSTANT_LOAD) * 4 + ((reg - TRANSFORM_CONSTANT) >> 2);
        if (g_trace && idx < 8) LOG_INFO("rsx", "  const write idx %u reg 0x%x load %u arg 0x%08x", idx, reg, R(TRANSFORM_CONSTANT_LOAD), arg);
        if (idx < 468 * 4) {
            std::memcpy(&g_vp_constants[idx], &arg, 4);
            g_vp_constants_dirty = true;
        }
        return;
    }
    switch (reg) {
    case INLINE_ARRAY:
        if (g_in_begin) g_inline.push_back(arg);
        return;
    case DRAW_ARRAYS:
    case DRAW_INDEX_ARRAY: {
        if (!g_in_begin) return;
        uint32_t first = arg & 0xFFFFFF, count = (arg >> 24) + 1;
        g_indexed = reg == DRAW_INDEX_ARRAY;
        if (!g_ranges.empty() && g_ranges.back().first + g_ranges.back().second == first) g_ranges.back().second += count;
        else g_ranges.push_back({first, count});
        return;
    }
    case ARRAY_ELEMENT16:
        if (g_in_begin) {
            g_elements.push_back(arg & 0xFFFF);
            g_elements.push_back(arg >> 16);
        }
        return;
    case ARRAY_ELEMENT32:
        if (g_in_begin) g_elements.push_back(arg);
        return;
    default: break;
    }
    g_regs[(reg >> 2) & 0x3FFF] = arg;
    if (reg == 0x17C8 && arg == 1) {
        g_zpass_draws.store(0, std::memory_order_relaxed);
        g_counter_queries.clear();
    }
    if (reg == BEGIN_END && !(arg & 0xFF) && g_in_begin && R(0x17CC)) g_zpass_draws.fetch_add(1, std::memory_order_relaxed);
    if (copy_engine(reg, arg)) return;
    if (immediate_method(reg, arg)) return;
    if (!enabled()) return;
    switch (reg) {
    case CLEAR_SURFACE: clear(arg); break;
    case BEGIN_END:
        if (arg & 0xFF) {
            g_prim = arg & 0xFF;
            g_in_begin = true;
            g_ranges.clear();
            g_inline.clear();
            g_elements.clear();
            g_indexed = false;
        } else if (g_in_begin) {
            g_in_begin = false;
            uint64_t t0 = ticks();
            if (!draw()) g_stats.skipped++;
            g_stats.draw_ticks += ticks() - t0;
        }
        break;
    default: break;
    }
}

namespace {

void mv_frame() {
    double elapsed = mv::elapsed();
    uint32_t vo = 0, zo = 0;
    if (!mv::enabled() || !mv::camera(vo, zo)) return;
    Surface* vel = nullptr;
    Surface* z = nullptr;
    for (Surface* s : surfaces_at(vo)) {
        if (!s->depth() && !s->main && format_bytes(s->format) == 4 && (!vel || s->generation > vel->generation)) vel = s;
    }
    for (Surface* s : surfaces_at(zo)) {
        if (s->format == VK_FORMAT_D24_UNORM_S8_UINT && !s->main && (!z || s->generation > z->generation)) z = s;
    }
    if (!vel || !z) return;
    static Surface* last_vel = nullptr;
    static Surface* last_z = nullptr;
    if (vel != last_vel || z != last_z) {
        LOG_INFO("mv", "inputs: velocity 0x%x %ux%u fmt %d, depth 0x%x %ux%u (%zu/%zu candidates)", vel->offset, vel->width, vel->height, int(vel->format), z->offset, z->width, z->height, surfaces_at(vo).size(), surfaces_at(zo).size());
        last_vel = vel;
        last_z = z;
    }
    transition(vel, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition(z, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    end_pass();
    int ran = mv::run(frame_cmd(), vel->view, depth_view(z), z->iw, z->ih, elapsed);
    if (ran == 2) g_mv_hudless = mv::dump_base(elapsed) + "_hudless.ppm";
    if (ran && dlssg::active()) {
        transition(z, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageLayout ml;
        VkImage mvi = mv::output(ml);
        dlssg::capture_inputs(frame_cmd(), mvi, ml, z->image, z->layout, z->iw, z->ih, mv::camera_consts());
    }
}

bool display_target(uint32_t offset) {
    for (uint32_t id = 0; id < 8; id++) {
        uint32_t o, p, w, h;
        if (display_buffer(id, o, p, w, h) && o == offset) return true;
    }
    return false;
}

void mv_draw(const std::vector<Surface*>& colors, uint32_t w, uint32_t h) {
    if (colors.empty() || !display_target(colors[0]->offset)) return;
    if (!g_mv_done) {
        g_mv_done = true;
        mv_frame();
        return;
    }
    if (dlssg::hudless_pending()) {
        transition(colors[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        dlssg::capture_hudless(frame_cmd(), colors[0]->image, colors[0]->layout, std::min(colors[0]->iw, uint32_t(w * colors[0]->scale + 0.5f)), std::min(colors[0]->ih, uint32_t(h * colors[0]->scale + 0.5f)));
    }
    if (!g_mv_hudless.empty()) {
        end_pass();
        schedule_snapshot(colors[0], w, h, g_mv_hudless);
        g_mv_hudless.clear();
    }
}

}

bool present(uint32_t buffer_id) {
    if (!enabled()) return false;
    end_pass();
    schedule_writebacks();
    static uint64_t presents = 0;
    static uint64_t trace_frame = std::getenv("GOWA_TRACE_FRAME") ? std::strtoull(std::getenv("GOWA_TRACE_FRAME"), nullptr, 0) : 0;
    ++presents;
    if (g_trace) {
        LOG_INFO("rsx", "trace present buffer %u", buffer_id);
        if (!std::getenv("GOWA_TRACE_DUMP_DRAW")) schedule_dumps();
    }
    g_trace = trace_frame && presents == trace_frame;
    static const double trace_at = std::getenv("GOWA_TRACE_AT") ? std::atof(std::getenv("GOWA_TRACE_AT")) : 0.0;
    static const auto run_start = std::chrono::steady_clock::now();
    static const double trace_at2 = std::getenv("GOWA_TRACE_AT2") ? std::atof(std::getenv("GOWA_TRACE_AT2")) : 0.0;
    static bool traced_at = false, traced_at2 = false;
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - run_start).count();
    if (trace_at > 0.0 && !traced_at && elapsed >= trace_at) {
        traced_at = true;
        g_trace = true;
    } else if (trace_at2 > 0.0 && !traced_at2 && elapsed >= trace_at2) {
        traced_at2 = true;
        g_trace = true;
    }
    g_mv_done = false;
    if (g_trace) g_trace_serial++;
    if (g_trace) g_trace_draws = 0;
    if (presents % 60 == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        double ms = 1000.0 / double(f.QuadPart) / 60.0;
        LOG_INFO("rsx", "renderer per frame: draw %.2f ms (textures %.2f ms), present %.2f ms", g_stats.draw_ticks * ms, g_stats.tex_ticks * ms, g_stats.present_ticks * ms);
        LOG_INFO("rsx", "  phases: programs %.2f indices %.2f textures+ubo %.2f (bind %.2f) vertices %.2f misc %.2f pipeline %.2f record %.2f", g_stats.phase[0] * ms, g_stats.phase[1] * ms, g_stats.phase[2] * ms,
                 g_stats.phase[7] * ms, g_stats.phase[3] * ms, g_stats.phase[4] * ms, g_stats.phase[5] * ms, g_stats.phase[6] * ms);
        std::memset(g_stats.phase, 0, sizeof g_stats.phase);
        LOG_INFO("rsx", "  copies %.1f per frame (%.1f MB), redirects %.1f", g_stats.copies / 60.0, g_stats.copy_bytes / 60.0 / 1048576.0, g_stats.redirects / 60.0);
        g_stats.copies = g_stats.copy_bytes = g_stats.redirects = 0;
        LOG_INFO("rsx", "  texture hash %.1f per frame (%.1f MB, %.2f ms), unchanged by watch %.1f, uploads %.1f (%.1f MB)", g_stats.hash_count / 60.0, g_stats.hash_bytes / 60.0 / 1048576.0, g_stats.hash_ticks * ms,
                 g_stats.watch_skips / 60.0, g_stats.upload_frame / 60.0, g_stats.upload_bytes / 60.0 / 1048576.0);
        g_stats.watch_skips = 0;
        LOG_INFO("rsx", "  nv3089 blits %.1f fast %.1f slow per frame", g_stats.blit_fast / 60.0, g_stats.blit_slow / 60.0);
        g_stats.blit_fast = g_stats.blit_slow = 0;
        static uint64_t last_faults = 0;
        uint64_t faults = vm::watch_faults();
        LOG_INFO("rsx", "  write watch faults %.1f per frame, stale repairs %llu, %s", (faults - last_faults) / 60.0, static_cast<unsigned long long>(vm::watch_stale_repairs()), vm::watch_top_pages().c_str());
        last_faults = faults;
        g_stats.hash_count = g_stats.hash_bytes = g_stats.hash_ticks = g_stats.upload_frame = g_stats.upload_bytes = 0;
        LOG_INFO("rsx", "  texture images: %.1f allocated, %.1f reused, %.1f freed per frame, %zu live", g_stats.image_allocs / 60.0, g_stats.pool_hits / 60.0, g_stats.textures_freed / 60.0, g_textures.size());
        {
            uint64_t tb = 0, sb = 0;
            for (auto& [k, t] : g_temps) tb += uint64_t(t->iw) * t->ih * 4;
            for (auto& [k, sp] : g_surfaces) sb += uint64_t(sp->iw) * sp->ih * 4;
            LOG_INFO("rsx", "  temps %zu (%.0f MB), surfaces %zu (%.0f MB), retired %zu", g_temps.size(), tb / 1048576.0, g_surfaces.size(), sb / 1048576.0, g_retired_surfaces.size());
        }
        g_stats.image_allocs = g_stats.pool_hits = g_stats.textures_freed = 0;
        g_stats.draw_ticks = g_stats.tex_ticks = g_stats.present_ticks = 0;
    }
    if (presents % 300 == 0 && g_stats.pipelines != g_pipelines_saved) {
        g_pipelines_saved = g_stats.pipelines;
        save_pipeline_cache();
    }
    if (presents % 600 == 0) {
        std::string sk;
        for (auto [l, c] : g_skip_lines) sk += " " + std::to_string(l) + ":" + std::to_string(c);
        if (!sk.empty()) LOG_INFO("rsx", "skip reasons:%s", sk.c_str());
        LOG_INFO("rsx", "renderer: %llu draws, %llu skipped, %llu vertices, %llu pipelines, %llu uploads, %zu textures, %zu surfaces", (unsigned long long)g_stats.draws,
                 (unsigned long long)g_stats.skipped, (unsigned long long)g_stats.vertices, (unsigned long long)g_stats.pipelines, (unsigned long long)g_stats.uploads, g_textures.size(),
                 g_surfaces.size());
    }
    uint32_t offset, pitch, width, height;
    if (!display_buffer(buffer_id, offset, pitch, width, height)) return false;
    static const char* show = std::getenv("GOWA_SHOW_SURFACE");
    if (show) {
        uint32_t want = static_cast<uint32_t>(std::strtoul(show, nullptr, 0));
        for (auto& [key, s] : g_surfaces) {
            if (s->offset == want && !s->depth()) {
                present_image(s->image, s->layout, s->iw, s->ih);
                return true;
            }
        }
    }
    static const double snap_every = std::getenv("GOWA_SNAP_EVERY") ? std::atof(std::getenv("GOWA_SNAP_EVERY")) : 0.0;
    static const double snap_from = std::getenv("GOWA_SNAP_FROM") ? std::atof(std::getenv("GOWA_SNAP_FROM")) : 0.0;
    static const std::string snap_dir = std::getenv("GOWA_SNAP_DIR") ? std::getenv("GOWA_SNAP_DIR") : ".";
    static double next_snap = snap_from;
    for (auto& [key, s] : g_surfaces) {
        if (s->offset == offset && !s->depth() && s->width >= width && s->height >= height) {
            if (snap_every > 0.0 && elapsed >= next_snap) {
                char name[64];
                std::snprintf(name, sizeof name, "/snap_%07.1f.ppm", elapsed);
                schedule_snapshot(s.get(), width, height, snap_dir + name);
                while (next_snap <= elapsed) next_snap += snap_every;
            }
            uint64_t pt = ticks();
            present_image(s->image, s->layout, width, height);
            g_stats.present_ticks += ticks() - pt;
            return true;
        }
    }
    return false;
}

}
