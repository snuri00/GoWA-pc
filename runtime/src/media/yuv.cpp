#include "runtime/video_decoder.h"

#include <cstring>
#include <immintrin.h>

namespace media {

namespace {

inline uint8_t clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

}

void i420_to_rgb32(const Frame& f, uint8_t* dst, size_t pitch, PixelOrder order, bool bt709, uint8_t alpha) {
    const int ky = 76309;
    const int krv = bt709 ? 117489 : 104597;
    const int kgu = bt709 ? 13954 : 25675;
    const int kgv = bt709 ? 34903 : 53279;
    const int kbu = bt709 ? 138438 : 132201;
    const int w = f.width, h = f.height, cw = w / 2;
    const int sa = order == PixelOrder::ARGB ? 0 : 24;
    const int sr = order == PixelOrder::ARGB ? 8 : 0;
    const int sg = order == PixelOrder::ARGB ? 16 : 8;
    const int sb = order == PixelOrder::ARGB ? 24 : 16;
    const __m256i vky = _mm256_set1_epi32(ky), vkrv = _mm256_set1_epi32(krv), vkgu = _mm256_set1_epi32(kgu), vkgv = _mm256_set1_epi32(kgv), vkbu = _mm256_set1_epi32(kbu);
    const __m256i c16 = _mm256_set1_epi32(16), c128 = _mm256_set1_epi32(128), zero = _mm256_setzero_si256(), c255 = _mm256_set1_epi32(255);
    const __m256i va = _mm256_set1_epi32(static_cast<int>(static_cast<uint32_t>(alpha) << sa));
    const __m256i dup = _mm256_setr_epi32(0, 0, 1, 1, 2, 2, 3, 3);
    const __m128i ssr = _mm_cvtsi32_si128(sr), ssg = _mm_cvtsi32_si128(sg), ssb = _mm_cvtsi32_si128(sb);
    for (int r = 0; r < h; r++) {
        const uint8_t* yp = &f.y[size_t(r) * w];
        const uint8_t* up = &f.u[size_t(r / 2) * cw];
        const uint8_t* vp = &f.v[size_t(r / 2) * cw];
        uint8_t* o = dst + size_t(r) * pitch;
        int x = 0;
        for (; x + 8 <= w; x += 8) {
            __m256i y = _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(yp + x)));
            int uu, vv;
            std::memcpy(&uu, up + x / 2, 4);
            std::memcpy(&vv, vp + x / 2, 4);
            __m256i u = _mm256_permutevar8x32_epi32(_mm256_cvtepu8_epi32(_mm_cvtsi32_si128(uu)), dup);
            __m256i v = _mm256_permutevar8x32_epi32(_mm256_cvtepu8_epi32(_mm_cvtsi32_si128(vv)), dup);
            y = _mm256_mullo_epi32(_mm256_sub_epi32(y, c16), vky);
            u = _mm256_sub_epi32(u, c128);
            v = _mm256_sub_epi32(v, c128);
            __m256i R = _mm256_srai_epi32(_mm256_add_epi32(y, _mm256_mullo_epi32(v, vkrv)), 16);
            __m256i G = _mm256_srai_epi32(_mm256_sub_epi32(y, _mm256_add_epi32(_mm256_mullo_epi32(u, vkgu), _mm256_mullo_epi32(v, vkgv))), 16);
            __m256i B = _mm256_srai_epi32(_mm256_add_epi32(y, _mm256_mullo_epi32(u, vkbu)), 16);
            R = _mm256_min_epi32(_mm256_max_epi32(R, zero), c255);
            G = _mm256_min_epi32(_mm256_max_epi32(G, zero), c255);
            B = _mm256_min_epi32(_mm256_max_epi32(B, zero), c255);
            __m256i px = _mm256_or_si256(_mm256_or_si256(va, _mm256_sll_epi32(R, ssr)), _mm256_or_si256(_mm256_sll_epi32(G, ssg), _mm256_sll_epi32(B, ssb)));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(o + 4 * x), px);
        }
        for (; x < w; x++) {
            int y = (yp[x] - 16) * ky;
            int u = up[x >> 1] - 128;
            int v = vp[x >> 1] - 128;
            uint32_t px = static_cast<uint32_t>(alpha) << sa;
            px |= static_cast<uint32_t>(clamp8((y + krv * v) >> 16)) << sr;
            px |= static_cast<uint32_t>(clamp8((y - kgu * u - kgv * v) >> 16)) << sg;
            px |= static_cast<uint32_t>(clamp8((y + kbu * u) >> 16)) << sb;
            std::memcpy(o + 4 * x, &px, 4);
        }
    }
}

}
