#include "edge_native.h"

#include <immintrin.h>

#include <cstring>
#include <vector>

namespace edge {

namespace {

inline uint32_t rd16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }

struct IdxTables {
    alignas(16) uint8_t ex[256][16];
    alignas(16) uint16_t rank0[256][8];
    alignas(16) uint8_t mask[256][16];
    alignas(32) uint8_t ctl[16][32];
    alignas(32) uint32_t sh[16][8];
    uint8_t off4[16];
    IdxTables() {
        std::memset(this, 0, sizeof *this);
        for (int b = 0; b < 256; b++) {
            int ones = 0, zeros = 0;
            for (int i = 0; i < 8; i++) {
                bool bit = (b >> (7 - i)) & 1;
                if (bit) {
                    ex[b][2 * i] = uint8_t(2 * ones);
                    ex[b][2 * i + 1] = uint8_t(2 * ones + 1);
                    mask[b][2 * i] = mask[b][2 * i + 1] = 0xff;
                    ones++;
                } else {
                    ex[b][2 * i] = ex[b][2 * i + 1] = 0x80;
                    rank0[b][i] = uint16_t(zeros);
                    zeros++;
                }
            }
        }
        for (int w = 1; w < 16; w++) {
            off4[w] = uint8_t((4 * w) >> 3);
            for (int k = 0; k < 8; k++) {
                int bit = k * w;
                int o = (bit >> 3) - (k >= 4 ? off4[w] : 0);
                for (int j = 0; j < 4; j++) ctl[w][4 * k + j] = uint8_t(o + 3 - j);
                sh[w][k] = uint32_t(32 - w - (bit & 7));
            }
        }
    }
};

const IdxTables& tables() {
    static const IdxTables t;
    return t;
}

struct Geo {
    uint32_t ntri, n_explicit, bias, flag_bytes, width, flags, codes, code_bytes, packed, ubase, groups, sbase, sgroups, cbase, tgroups;
};

bool geometry(const uint8_t* ls, uint32_t ctx, uint32_t src, Geo& g) {
    if ((src & 15) || ctx + 512 >= 0x40000 || src + 64 >= 0x40000) return false;
    g.ntri = (rd16(ls + ctx + 400) * 0xaaabu) & 0xffff;
    const uint8_t* h = ls + src;
    g.n_explicit = rd16(h);
    g.bias = rd16(h + 2);
    g.flag_bytes = rd16(h + 4);
    g.width = h[6];
    if (g.width == 0 || g.width > 15 || g.n_explicit == 0 || g.ntri == 0) return false;
    g.flags = src + 8;
    g.codes = g.flags + g.flag_bytes;
    g.code_bytes = (2 * g.ntri + 7) >> 3;
    g.packed = g.codes + g.code_bytes;
    g.ubase = src + ((8 + g.flag_bytes + g.code_bytes + 15) & ~15u);
    g.groups = (g.n_explicit + 31) >> 5;
    g.sbase = src + ((6 * g.ntri + 15) & ~15u);
    g.sgroups = (8 * g.flag_bytes + 15) >> 4;
    if (!g.sgroups) return false;
    g.cbase = g.sbase + 32 * g.sgroups;
    g.tgroups = (g.ntri + 7) >> 3;
    if (g.sbase < g.ubase + 64 * g.groups + 64 || g.sbase < g.ubase + 32 * g.sgroups + 32) return false;
    if (g.ntri < 32 || src + 48 * g.tgroups > g.sbase + 48) return false;
    uint64_t hi = g.cbase + 16ull * (((g.code_bytes + 15) >> 4) + 1);
    uint64_t shi = uint64_t(g.sbase) + 48ull * g.tgroups + 16;
    if (shi > hi) hi = shi;
    uint64_t phi = uint64_t(g.packed) + 4ull * g.groups * g.width + 32;
    if (phi > hi) hi = phi;
    return hi + 64 < 0x40000;
}

}

bool decompress_indexes_fast(uint8_t* ls, uint32_t ctx, uint32_t src, uint32_t* end_out) {
    Geo g;
    if (!geometry(ls, ctx, src, g)) return false;
    const IdxTables& T = tables();
    const __m128i swap16 = _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
    thread_local std::vector<uint16_t> tmp;
    if (tmp.size() < size_t(g.groups) * 32 + 16) tmp.resize(size_t(g.groups) * 32 + 16);
    {
        const __m256i ctl = _mm256_load_si256(reinterpret_cast<const __m256i*>(T.ctl[g.width]));
        const __m256i sh = _mm256_load_si256(reinterpret_cast<const __m256i*>(T.sh[g.width]));
        const __m256i mask = _mm256_set1_epi32(int((1u << g.width) - 1));
        const __m128i biasv = _mm_set1_epi16(short(g.bias));
        uint32_t o4 = T.off4[g.width];
        __m128i carry = _mm_setzero_si128();
        const uint8_t* pk = ls + g.packed;
        uint16_t* out = tmp.data();
        for (uint32_t blk = 0; blk < g.groups * 4; blk++) {
            const uint8_t* p = pk + blk * g.width;
            __m256i v = _mm256_loadu2_m128i(reinterpret_cast<const __m128i*>(p + o4), reinterpret_cast<const __m128i*>(p));
            v = _mm256_shuffle_epi8(v, ctl);
            v = _mm256_and_si256(_mm256_srlv_epi32(v, sh), mask);
            __m128i row = _mm_packus_epi32(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1));
            carry = _mm_add_epi16(carry, _mm_sub_epi16(row, biasv));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 8 * blk), _mm_shuffle_epi8(carry, swap16));
        }
        std::memcpy(ls + g.ubase, tmp.data(), size_t(g.groups) * 64);
    }
    {
        uint32_t seq = 0;
        const uint8_t* fl = ls + g.flags;
        const uint8_t* ex = ls + g.ubase;
        uint8_t* out = ls + g.sbase;
        for (uint32_t k = 0; k < 2 * g.sgroups; k++) {
            uint32_t b = fl[k];
            __m128i e = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(ex)), _mm_load_si128(reinterpret_cast<const __m128i*>(T.ex[b])));
            __m128i sv = _mm_add_epi16(_mm_set1_epi16(short(seq)), _mm_load_si128(reinterpret_cast<const __m128i*>(T.rank0[b])));
            sv = _mm_shuffle_epi8(sv, swap16);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out), _mm_blendv_epi8(sv, e, _mm_load_si128(reinterpret_cast<const __m128i*>(T.mask[b]))));
            out += 16;
            uint32_t ones = uint32_t(__builtin_popcount(b));
            ex += 2 * ones;
            seq += 8 - ones;
        }
    }
    {
        uint32_t k16 = (g.code_bytes + 15) >> 4;
        std::memmove(ls + g.cbase, ls + g.codes, size_t(k16) * 16);
    }
    {
        alignas(16) static const uint8_t ctl[4][16] = {
            {0, 1, 4, 5, 8, 9, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
            {4, 5, 2, 3, 8, 9, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
            {2, 3, 0, 1, 8, 9, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
            {8, 9, 10, 11, 12, 13, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
        };
        const uint8_t* sp = ls + g.sbase;
        const uint8_t* cw = ls + g.cbase;
        uint8_t* out = ls + src;
        uint8_t* out_end = out + 48 * g.tgroups;
        uint16_t keep;
        std::memcpy(&keep, out_end, 2);
        __m128i P = _mm_setzero_si128();
        for (uint32_t gi = 0; gi < g.tgroups; gi++) {
            uint32_t word = rd16(cw + 2 * gi);
            for (uint32_t t = 0; t < 8; t++) {
                uint32_t code = (word >> (14 - 2 * t)) & 3;
                __m128i S = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(sp));
                P = _mm_shuffle_epi8(_mm_unpacklo_epi64(P, S), _mm_load_si128(reinterpret_cast<const __m128i*>(ctl[code])));
                _mm_storel_epi64(reinterpret_cast<__m128i*>(out), P);
                out += 6;
                sp += code == 3 ? 6 : 2;
            }
        }
        std::memcpy(out_end, &keep, 2);
    }
    uint32_t end = (src + 48 * g.tgroups + 15) & ~15u;
    uint32_t be = __builtin_bswap32(end);
    std::memcpy(ls + ctx + 88, &be, 4);
    if (end_out) *end_out = end;
    return true;
}

}
