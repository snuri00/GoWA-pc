#include "texture_decode.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <immintrin.h>

namespace gpu {

namespace {

enum Conv { COPY, SWAP16, SWAP32, R6G5B5, B8R8G8R8, R8B8R8G8, D24, D24F, E4M12 };

struct FormatInfo {
    uint32_t code;
    uint32_t bytes;
    uint32_t block;
    VkFormat vk;
    Conv conv;
    VkComponentSwizzle base[4];
    bool remap;
    uint32_t out_bytes;
};

constexpr VkComponentSwizzle R = VK_COMPONENT_SWIZZLE_R;
constexpr VkComponentSwizzle G = VK_COMPONENT_SWIZZLE_G;
constexpr VkComponentSwizzle B = VK_COMPONENT_SWIZZLE_B;
constexpr VkComponentSwizzle A = VK_COMPONENT_SWIZZLE_A;
constexpr VkComponentSwizzle I = VK_COMPONENT_SWIZZLE_ONE;
constexpr VkComponentSwizzle Z = VK_COMPONENT_SWIZZLE_ZERO;

const FormatInfo FORMATS[] = {
    {0x81, 1, 1, VK_FORMAT_R8_UNORM, COPY, {I, R, R, R}, true, 1},
    {0x82, 2, 1, VK_FORMAT_A1R5G5B5_UNORM_PACK16, SWAP16, {A, R, G, B}, true, 2},
    {0x83, 2, 1, VK_FORMAT_R4G4B4A4_UNORM_PACK16, SWAP16, {R, G, B, A}, true, 2},
    {0x84, 2, 1, VK_FORMAT_R5G6B5_UNORM_PACK16, SWAP16, {I, R, G, B}, true, 2},
    {0x85, 4, 1, VK_FORMAT_B8G8R8A8_UNORM, SWAP32, {A, R, G, B}, true, 4},
    {0x86, 8, 4, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, COPY, {A, R, G, B}, true, 8},
    {0x87, 16, 4, VK_FORMAT_BC2_UNORM_BLOCK, COPY, {A, R, G, B}, true, 16},
    {0x88, 16, 4, VK_FORMAT_BC3_UNORM_BLOCK, COPY, {A, R, G, B}, true, 16},
    {0x8B, 2, 1, VK_FORMAT_R8G8_UNORM, SWAP16, {G, R, G, R}, true, 2},
    {0x8D, 4, 2, VK_FORMAT_B8G8R8A8_UNORM, B8R8G8R8, {A, R, G, B}, true, 8},
    {0x8E, 4, 2, VK_FORMAT_B8G8R8A8_UNORM, R8B8R8G8, {A, R, G, B}, true, 8},
    {0x8F, 2, 1, VK_FORMAT_B8G8R8A8_UNORM, R6G5B5, {I, R, G, B}, true, 4},
    {0x90, 4, 1, VK_FORMAT_R32_SFLOAT, D24, {R, R, R, R}, false, 4},
    {0x91, 4, 1, VK_FORMAT_R32_SFLOAT, D24F, {R, R, R, R}, false, 4},
    {0x92, 2, 1, VK_FORMAT_R16_UNORM, SWAP16, {R, R, R, R}, false, 2},
    {0x93, 2, 1, VK_FORMAT_R32_SFLOAT, E4M12, {R, R, R, R}, false, 4},
    {0x94, 2, 1, VK_FORMAT_R16_UNORM, SWAP16, {R, I, R, I}, false, 2},
    {0x95, 4, 1, VK_FORMAT_R16G16_UNORM, SWAP16, {G, R, G, R}, false, 4},
    {0x97, 2, 1, VK_FORMAT_R5G5B5A1_UNORM_PACK16, SWAP16, {A, R, G, B}, true, 2},
    {0x98, 2, 1, VK_FORMAT_R8G8_UNORM, COPY, {G, R, G, R}, false, 2},
    {0x99, 2, 1, VK_FORMAT_R8G8_SNORM, COPY, {G, R, G, R}, false, 2},
    {0x9A, 8, 1, VK_FORMAT_R16G16B16A16_SFLOAT, SWAP16, {A, R, G, B}, false, 8},
    {0x9B, 16, 1, VK_FORMAT_R32G32B32A32_SFLOAT, SWAP32, {A, R, G, B}, false, 16},
    {0x9C, 4, 1, VK_FORMAT_R32_SFLOAT, SWAP32, {R, R, R, R}, false, 4},
    {0x9D, 2, 1, VK_FORMAT_A1R5G5B5_UNORM_PACK16, SWAP16, {I, R, G, B}, true, 2},
    {0x9E, 4, 1, VK_FORMAT_B8G8R8A8_UNORM, SWAP32, {I, R, G, B}, true, 4},
    {0x9F, 4, 1, VK_FORMAT_R16G16_SFLOAT, SWAP16, {G, R, G, R}, false, 4},
};

const FormatInfo* find_format(uint32_t code) {
    for (const FormatInfo& f : FORMATS) {
        if (f.code == code) return &f;
    }
    return nullptr;
}

uint32_t ceil_log2(uint32_t v) {
    uint32_t r = 0;
    while ((1u << r) < v) r++;
    return r;
}

uint32_t floor_log2(uint32_t v) {
    uint32_t r = 0;
    while (v > 1) {
        v >>= 1;
        r++;
    }
    return r;
}

struct LevelGeom {
    uint32_t w, h, bw, bh, src_pitch, src_size;
};

LevelGeom level_geom(const FormatInfo& f, uint32_t w, uint32_t h, uint32_t level, bool linear, uint32_t pitch) {
    LevelGeom g{};
    g.w = std::max(1u, w >> level);
    g.h = std::max(1u, h >> level);
    if (f.block == 4) {
        g.bw = (g.w + 3) / 4;
        g.bh = (g.h + 3) / 4;
    } else if (f.block == 2) {
        g.bw = (g.w + 1) / 2;
        g.bh = g.h;
    } else {
        g.bw = g.w;
        g.bh = g.h;
    }
    uint32_t packed = g.bw * f.bytes;
    g.src_pitch = linear && pitch ? pitch : packed;
    g.src_size = g.src_pitch * g.bh;
    return g;
}

uint16_t load16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t load32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

void convert_unit(const FormatInfo& f, const uint8_t* s, uint8_t* d) {
    switch (f.conv) {
    case COPY: std::memcpy(d, s, f.bytes); break;
    case SWAP16:
        for (uint32_t i = 0; i < f.bytes; i += 2) {
            d[i] = s[i + 1];
            d[i + 1] = s[i];
        }
        break;
    case SWAP32:
        for (uint32_t i = 0; i < f.bytes; i += 4) {
            d[i] = s[i + 3];
            d[i + 1] = s[i + 2];
            d[i + 2] = s[i + 1];
            d[i + 3] = s[i];
        }
        break;
    case R6G5B5: {
        uint16_t v = load16(s);
        uint32_t r = (v >> 10) & 0x3F, g = (v >> 5) & 0x1F, b = v & 0x1F;
        d[0] = static_cast<uint8_t>((b << 3) | (b >> 2));
        d[1] = static_cast<uint8_t>((g << 3) | (g >> 2));
        d[2] = static_cast<uint8_t>((r << 2) | (r >> 4));
        d[3] = 0xFF;
        break;
    }
    case B8R8G8R8:
    case R8B8R8G8: {
        uint8_t b, g, r0, r1;
        if (f.conv == B8R8G8R8) {
            b = s[0];
            r0 = s[1];
            g = s[2];
            r1 = s[3];
        } else {
            r0 = s[0];
            b = s[1];
            r1 = s[2];
            g = s[3];
        }
        d[0] = b;
        d[1] = g;
        d[2] = r0;
        d[3] = 0xFF;
        d[4] = b;
        d[5] = g;
        d[6] = r1;
        d[7] = 0xFF;
        break;
    }
    case D24: {
        float v = (load32(s) >> 8) / 16777215.0f;
        std::memcpy(d, &v, 4);
        break;
    }
    case D24F: {
        uint32_t bits = (load32(s) >> 8) << 7;
        std::memcpy(d, &bits, 4);
        break;
    }
    case E4M12: {
        uint16_t v = load16(s);
        uint32_t e = v >> 12, m = v & 0xFFF;
        float r = e ? std::ldexp(1.0f + m / 4096.0f, int(e) - 15) : std::ldexp(m / 4096.0f, -14);
        std::memcpy(d, &r, 4);
        break;
    }
    }
}

void swap16_run(const uint8_t* s, uint8_t* d, size_t n) {
    size_t i = 0;
#ifdef __AVX2__
    const __m256i m = _mm256_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14, 1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
    for (; i + 32 <= n; i += 32) _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + i), _mm256_shuffle_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + i)), m));
#endif
    for (; i + 2 <= n; i += 2) {
        d[i] = s[i + 1];
        d[i + 1] = s[i];
    }
}

void swap32_run(const uint8_t* s, uint8_t* d, size_t n) {
    size_t i = 0;
#ifdef __AVX2__
    const __m256i m = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    for (; i + 32 <= n; i += 32) _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + i), _mm256_shuffle_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + i)), m));
#endif
    for (; i + 4 <= n; i += 4) {
        d[i] = s[i + 3];
        d[i + 1] = s[i + 2];
        d[i + 2] = s[i + 1];
        d[i + 3] = s[i];
    }
}

bool run_capable(const FormatInfo& f) { return (f.conv == COPY || f.conv == SWAP16 || f.conv == SWAP32) && f.out_bytes == f.bytes; }

void convert_run(const FormatInfo& f, const uint8_t* s, uint8_t* d, uint32_t units) {
    if (run_capable(f)) {
        size_t n = size_t(units) * f.bytes;
        if (f.conv == COPY) std::memcpy(d, s, n);
        else if (f.conv == SWAP16) swap16_run(s, d, n);
        else swap32_run(s, d, n);
        return;
    }
    for (uint32_t i = 0; i < units; i++) convert_unit(f, s + size_t(i) * f.bytes, d + size_t(i) * f.out_bytes);
}

template <class Load>
void swizzled_rows(const uint32_t* xs, const uint32_t* ys, uint32_t bw, uint32_t bh, Load load) {
    for (uint32_t y = 0; y < bh; y++) {
        uint32_t yo = ys[y];
        for (uint32_t x = 0; x < bw; x++) load(x, y, xs[x] | yo);
    }
}

void decode_swizzled(const FormatInfo& f, const uint8_t* s, uint8_t* d, uint32_t bw, uint32_t bh, const std::vector<uint32_t>& xs, const std::vector<uint32_t>& ys) {
    if (run_capable(f) && f.bytes == 4) {
        uint32_t* o = reinterpret_cast<uint32_t*>(d);
        if (f.conv == SWAP32) {
            swizzled_rows(xs.data(), ys.data(), bw, bh, [&](uint32_t x, uint32_t y, uint32_t m) {
                uint32_t v;
                std::memcpy(&v, s + size_t(m) * 4, 4);
                v = __builtin_bswap32(v);
                std::memcpy(o + size_t(y) * bw + x, &v, 4);
            });
        } else if (f.conv == SWAP16) {
            swizzled_rows(xs.data(), ys.data(), bw, bh, [&](uint32_t x, uint32_t y, uint32_t m) {
                uint32_t v;
                std::memcpy(&v, s + size_t(m) * 4, 4);
                v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
                std::memcpy(o + size_t(y) * bw + x, &v, 4);
            });
        } else {
            swizzled_rows(xs.data(), ys.data(), bw, bh, [&](uint32_t x, uint32_t y, uint32_t m) { std::memcpy(o + size_t(y) * bw + x, s + size_t(m) * 4, 4); });
        }
        return;
    }
    if (run_capable(f) && f.bytes == 2) {
        bool swap = f.conv == SWAP16;
        swizzled_rows(xs.data(), ys.data(), bw, bh, [&](uint32_t x, uint32_t y, uint32_t m) {
            uint16_t v;
            std::memcpy(&v, s + size_t(m) * 2, 2);
            if (swap) v = __builtin_bswap16(v);
            std::memcpy(d + (size_t(y) * bw + x) * 2, &v, 2);
        });
        return;
    }
    if (run_capable(f) && f.bytes == 1) {
        swizzled_rows(xs.data(), ys.data(), bw, bh, [&](uint32_t x, uint32_t y, uint32_t m) { d[size_t(y) * bw + x] = s[m]; });
        return;
    }
    swizzled_rows(xs.data(), ys.data(), bw, bh, [&](uint32_t x, uint32_t y, uint32_t m) { convert_unit(f, s + uint64_t(m) * f.bytes, d + (uint64_t(y) * bw + x) * f.out_bytes); });
}

void morton_tables(uint32_t w, uint32_t h, std::vector<uint32_t>& xs, std::vector<uint32_t>& ys) {
    uint32_t a = ceil_log2(w), b = ceil_log2(h), m = std::min(a, b);
    xs.assign(w, 0);
    ys.assign(h, 0);
    for (uint32_t x = 0; x < w; x++) {
        uint32_t v = 0;
        for (uint32_t k = 0; k < a; k++) {
            if (!((x >> k) & 1)) continue;
            v |= k < m ? 1u << (2 * k) : 1u << (k + m);
        }
        xs[x] = v;
    }
    for (uint32_t y = 0; y < h; y++) {
        uint32_t v = 0;
        for (uint32_t k = 0; k < b; k++) {
            if (!((y >> k) & 1)) continue;
            v |= k < m ? 1u << (2 * k + 1) : 1u << (k + m);
        }
        ys[y] = v;
    }
}

}

VkComponentMapping remap_mapping(const VkComponentSwizzle base[4], bool remap, uint32_t control1) {
    VkComponentSwizzle out[4];
    for (uint32_t i = 0; i < 4; i++) {
        if (!remap) {
            out[i] = base[i];
            continue;
        }
        uint32_t sel = (control1 >> (2 * i)) & 3;
        uint32_t ctl = (control1 >> (8 + 2 * i)) & 3;
        out[i] = ctl == 0 ? Z : ctl == 1 ? I : base[sel];
    }
    return {out[1], out[2], out[3], out[0]};
}

VkComponentMapping texture_mapping(uint32_t base_format, uint32_t control1) {
    const FormatInfo* f = find_format(base_format);
    static const VkComponentSwizzle identity[4] = {A, R, G, B};
    if (!f) return remap_mapping(identity, true, control1);
    return remap_mapping(f->base, f->remap, control1);
}

bool describe_texture(const TextureRegs& regs, TextureLayout& out) {
    uint32_t fmt = (regs.format >> 8) & 0xFF;
    uint32_t code = fmt & ~0x60u;
    const FormatInfo* f = find_format(code);
    if (!f) return false;
    bool linear = fmt & 0x20;
    uint32_t dim = (regs.format >> 4) & 0xF;
    bool cube = (regs.format >> 2) & 1;
    uint32_t w = regs.rect >> 16;
    uint32_t h = dim == 1 ? 1 : regs.rect & 0xFFFF;
    if (!w || !h) return false;
    uint32_t depth = dim == 3 ? std::max(1u, regs.control3 >> 20) : 1;
    uint32_t mips = std::max(1u, regs.format >> 16);
    uint32_t max_levels = f->block == 4 ? floor_log2(std::max(1u, std::min(w / 4, h / 4))) + 1 : floor_log2(std::max({w, h, depth})) + 1;
    out.levels = std::min(mips, max_levels);
    out.layers = cube && dim == 2 ? 6 : 1;
    out.view_type = dim == 1 ? VK_IMAGE_VIEW_TYPE_1D : dim == 3 ? VK_IMAGE_VIEW_TYPE_3D : out.layers == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    out.width = w;
    out.height = h;
    out.depth = depth;
    out.format = f->vk;
    out.is_depth = code >= 0x90 && code <= 0x93;
    uint32_t gamma = (regs.address >> 20) & 0xF;
    if ((gamma & 7) == 7) {
        if (out.format == VK_FORMAT_B8G8R8A8_UNORM) out.format = VK_FORMAT_B8G8R8A8_SRGB;
        else if (out.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK) out.format = VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        else if (out.format == VK_FORMAT_BC2_UNORM_BLOCK) out.format = VK_FORMAT_BC2_SRGB_BLOCK;
        else if (out.format == VK_FORMAT_BC3_UNORM_BLOCK) out.format = VK_FORMAT_BC3_SRGB_BLOCK;
    }
    out.mapping = texture_mapping(code, regs.control1);
    out.regions.clear();
    uint32_t pitch = regs.control3 & 0xFFFFF;
    uint64_t src = 0, dst = 0;
    for (uint32_t layer = 0; layer < out.layers; layer++) {
        for (uint32_t level = 0; level < out.levels; level++) {
            LevelGeom g = level_geom(*f, w, h, level, linear, pitch);
            TextureRegion r;
            r.offset = dst;
            r.level = level;
            r.layer = layer;
            r.width = g.w;
            r.height = g.h;
            r.depth = std::max(1u, depth >> level);
            out.regions.push_back(r);
            src += uint64_t(g.src_size) * r.depth;
            dst += (uint64_t(g.bw) * g.bh * r.depth * f->out_bytes + 15) & ~15ull;
        }
        if (!linear) src = (src + 127) & ~127ull;
    }
    out.source_size = static_cast<uint32_t>(src);
    out.decoded_size = dst;
    return true;
}

void decode_texture(const uint8_t* src, const TextureRegs& regs, const TextureLayout& layout, uint8_t* dst) {
    uint32_t fmt = (regs.format >> 8) & 0xFF;
    const FormatInfo* f = find_format(fmt & ~0x60u);
    if (!f) return;
    bool linear = fmt & 0x20;
    uint32_t pitch = regs.control3 & 0xFFFFF;
    uint64_t so = 0;
    std::vector<uint32_t> xs, ys;
    size_t ri = 0;
    for (uint32_t layer = 0; layer < layout.layers; layer++) {
        for (uint32_t level = 0; level < layout.levels; level++, ri++) {
            LevelGeom g = level_geom(*f, layout.width, layout.height, level, linear, pitch);
            const uint8_t* s = src + so;
            uint8_t* d = dst + layout.regions[ri].offset;
            uint32_t slices = layout.regions[ri].depth;
            if (slices > 1) {
                uint32_t lw = floor_log2(g.bw), lh = floor_log2(g.bh), ld = floor_log2(slices);
                for (uint32_t z = 0; z < slices; z++) {
                    for (uint32_t y = 0; y < g.bh; y++) {
                        for (uint32_t x = 0; x < g.bw; x++) {
                            uint64_t at;
                            if (linear || f->block == 4) {
                                at = uint64_t(z) * g.src_size + uint64_t(y) * g.src_pitch + uint64_t(x) * f->bytes;
                            } else {
                                uint64_t m = 0;
                                uint32_t bit = 0;
                                for (uint32_t k = 0; k < std::max({lw, lh, ld}); k++) {
                                    if (k < lw) m |= uint64_t((x >> k) & 1) << bit++;
                                    if (k < lh) m |= uint64_t((y >> k) & 1) << bit++;
                                    if (k < ld) m |= uint64_t((z >> k) & 1) << bit++;
                                }
                                at = m * f->bytes;
                            }
                            convert_unit(*f, s + at, d + ((uint64_t(z) * g.bh + y) * g.bw + x) * f->out_bytes);
                        }
                    }
                }
            } else if (linear || f->block == 4) {
                if (g.src_pitch == g.bw * f->bytes) {
                    convert_run(*f, s, d, g.bw * g.bh);
                } else {
                    for (uint32_t y = 0; y < g.bh; y++) convert_run(*f, s + size_t(y) * g.src_pitch, d + uint64_t(y) * g.bw * f->out_bytes, g.bw);
                }
            } else if (f->bytes > 4) {
                uint32_t sub = f->bytes / 4;
                uint32_t uw = g.bw * sub;
                morton_tables(uw, g.bh, xs, ys);
                std::vector<uint8_t> tmp(f->bytes);
                for (uint32_t y = 0; y < g.bh; y++) {
                    for (uint32_t x = 0; x < g.bw; x++) {
                        for (uint32_t k = 0; k < sub; k++) std::memcpy(&tmp[k * 4], s + uint64_t(xs[x * sub + k] | ys[y]) * 4, 4);
                        convert_unit(*f, tmp.data(), d + (uint64_t(y) * g.bw + x) * f->out_bytes);
                    }
                }
            } else {
                morton_tables(g.bw, g.bh, xs, ys);
                decode_swizzled(*f, s, d, g.bw, g.bh, xs, ys);
            }
            so += uint64_t(g.src_size) * slices;
        }
        if (!linear) so = (so + 127) & ~127ull;
    }
}

}
