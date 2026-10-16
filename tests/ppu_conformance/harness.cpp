#include "conformance.h"
#include "ppu/ops.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr size_t SLOT = 0x440;
constexpr size_t OFF_FPR = 0x100, OFF_VR = 0x200, OFF_CR = 0x400, OFF_XER = 0x408, OFF_CTR = 0x410, OFF_LR = 0x418, OFF_FPSCR = 0x420, OFF_VSCR = 0x430;

uint64_t rd64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return __builtin_bswap64(v); }
uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return __builtin_bswap32(v); }
void wr64(uint8_t* p, uint64_t v) { v = __builtin_bswap64(v); std::memcpy(p, &v, 8); }
void wr32(uint8_t* p, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(p, &v, 4); }

void unpack(const uint8_t* s, ppu::Context& c) {
    std::memset(&c, 0, sizeof(c));
    for (int n = 0; n < 32; n++) {
        c.gpr[n] = rd64(s + 8 * n);
        c.fpr[n] = std::bit_cast<double>(rd64(s + OFF_FPR + 8 * n));
        for (int i = 0; i < 16; i++) c.vr[n].u8[15 - i] = s[OFF_VR + 16 * n + i];
    }
    ppu::cr_put(c, 0xFF, rd32(s + OFF_CR));
    ppu::xer_put(c, rd64(s + OFF_XER));
    c.ctr = rd64(s + OFF_CTR);
    c.lr = rd64(s + OFF_LR);
    c.fpscr = static_cast<uint32_t>(rd64(s + OFF_FPSCR));
    uint32_t vscr = rd32(s + OFF_VSCR + 12);
    c.vscr_nj = vscr >> 16 & 1;
    c.vscr_sat = vscr & 1;
}

void pack(const ppu::Context& c, uint8_t* s) {
    std::memset(s, 0, SLOT);
    for (int n = 0; n < 32; n++) {
        wr64(s + 8 * n, c.gpr[n]);
        wr64(s + OFF_FPR + 8 * n, std::bit_cast<uint64_t>(c.fpr[n]));
        for (int i = 0; i < 16; i++) s[OFF_VR + 16 * n + i] = c.vr[n].u8[15 - i];
    }
    wr32(s + OFF_CR, ppu::cr_get(c));
    wr64(s + OFF_XER, ppu::xer_get(c));
    wr64(s + OFF_CTR, c.ctr);
    wr64(s + OFF_LR, c.lr);
    wr64(s + OFF_FPSCR, c.fpscr);
    wr32(s + OFF_VSCR + 12, c.vscr_nj << 16 | c.vscr_sat);
}

std::string field_name(size_t off) {
    char b[32];
    if (off < OFF_FPR) std::snprintf(b, sizeof b, "r%zu", off / 8);
    else if (off < OFF_VR) std::snprintf(b, sizeof b, "f%zu", (off - OFF_FPR) / 8);
    else if (off < OFF_CR) std::snprintf(b, sizeof b, "v%zu", (off - OFF_VR) / 16);
    else if (off < OFF_XER) return "cr";
    else if (off < OFF_CTR) return "xer";
    else if (off < OFF_LR) return "ctr";
    else if (off < OFF_FPSCR) return "lr";
    else if (off < OFF_VSCR) return "fpscr";
    else return "vscr";
    return b;
}

size_t field_size(size_t off) {
    if (off >= OFF_VR && off < OFF_CR) return 16;
    if (off >= OFF_VSCR) return 16;
    return 8;
}

std::vector<uint8_t> read_file(const char* path) {
    std::vector<uint8_t> v;
    FILE* f = std::fopen(path, "rb");
    if (!f) return v;
    std::fseek(f, 0, SEEK_END);
    v.resize(std::ftell(f));
    std::fseek(f, 0, SEEK_SET);
    std::fread(v.data(), 1, v.size(), f);
    std::fclose(f);
    return v;
}

}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: ppu_conformance inputs.bin rpcs3_output.bin [ignore_fpscr]\n");
        return 2;
    }
    bool ignore_fpscr = argc > 3;
    auto in = read_file(argv[1]);
    auto ref = read_file(argv[2]);
    size_t slots = static_cast<size_t>(conformance_test_count) * conformance_states;
    if (in.size() != slots * SLOT || ref.size() != slots * SLOT) {
        std::fprintf(stderr, "size mismatch: inputs %zu reference %zu expected %zu\n", in.size(), ref.size(), slots * SLOT);
        return 2;
    }
    static uint8_t memory[16];
    std::map<std::string, std::pair<int, int>> stats;
    std::map<std::string, int> shown_per;
    std::map<std::string, int> tolerated;
    std::vector<uint8_t> got(SLOT);
    for (uint32_t t = 0; t < conformance_test_count; t++) {
        const ConformanceTest& ct = conformance_tests[t];
        for (uint32_t s = 0; s < conformance_states; s++) {
            size_t slot = static_cast<size_t>(t) * conformance_states + s;
            ppu::Context ctx;
            unpack(in.data() + slot * SLOT, ctx);
            ct.fn(ctx, memory);
            pack(ctx, got.data());
            const uint8_t* want = ref.data() + slot * SLOT;
            std::vector<uint8_t> want_norm;
            std::vector<size_t> bad;
            uint32_t gv = 0, wv = 0;
            for (int k = 0; k < 4; k++) {
                gv |= rd32(got.data() + OFF_VSCR + 4 * k);
                wv |= rd32(want + OFF_VSCR + 4 * k);
            }
            for (int k = 0; k < 4; k++) {
                wr32(got.data() + OFF_VSCR + 4 * k, k == 3 ? (gv & ~1u) : 0);
            }
            want_norm.assign(want, want + SLOT);
            for (int k = 0; k < 4; k++) wr32(want_norm.data() + OFF_VSCR + 4 * k, k == 3 ? (wv & ~1u) : 0);
            want = want_norm.data();
            for (size_t off = 0; off < SLOT;) {
                size_t sz = field_size(off);
                bool skip = (off == 8 * ct.base_reg) || (ignore_fpscr && off == OFF_FPSCR);
                if (!skip && std::memcmp(got.data() + off, want + off, sz) != 0) {
                    if (off >= OFF_FPR && off < OFF_VR && std::isnan(std::bit_cast<double>(rd64(got.data() + off))) && std::isnan(std::bit_cast<double>(rd64(want + off)))) {
                        tolerated[ct.mnemonic]++;
                    } else {
                        bad.push_back(off);
                    }
                }
                off += sz;
            }
            auto& st = stats[ct.mnemonic];
            st.first++;
            if (!bad.empty()) {
                st.second++;
                if (shown_per[ct.mnemonic]++ < 3) {
                    std::printf("MISMATCH %-10s %08x state %u:", ct.mnemonic, ct.word, s);
                    for (size_t off : bad) {
                        std::printf(" %s got=", field_name(off).c_str());
                        for (size_t i = 0; i < field_size(off); i++) std::printf("%02x", got[off + i]);
                        std::printf(" want=");
                        for (size_t i = 0; i < field_size(off); i++) std::printf("%02x", want[off + i]);
                        std::printf(" in=");
                        for (size_t i = 0; i < field_size(off); i++) std::printf("%02x", in[slot * SLOT + off + i]);
                    }
                    const uint8_t* src = in.data() + slot * SLOT;
                    bool vec = ct.mnemonic[0] == 'v';
                    bool fp = ct.mnemonic[0] == 'f';
                    if (vec || fp) {
                        const char* names[3] = {"A", "B", "C"};
                        uint32_t idx[3] = {(ct.word >> 16) & 31, (ct.word >> 11) & 31, (ct.word >> 6) & 31};
                        for (int k = 0; k < 3; k++) {
                            std::printf(" %s=", names[k]);
                            size_t base_off = vec ? OFF_VR + 16 * idx[k] : OFF_FPR + 8 * idx[k];
                            for (size_t i = 0; i < (vec ? 16u : 8u); i++) std::printf("%02x", src[base_off + i]);
                        }
                        std::printf(" vscr=%08x", rd32(src + OFF_VSCR + 12));
                    }
                    std::printf("\n");
                }
            }
        }
    }
    int total = 0, failed = 0, mfail = 0;
    for (auto& [m, st] : stats) {
        total += st.first;
        failed += st.second;
        if (st.second) {
            mfail++;
            std::printf("  %-12s %d/%d failed\n", m.c_str(), st.second, st.first);
        }
    }
    int tol = 0;
    for (auto& [m, n] : tolerated) {
        tol += n;
        std::printf("  %-12s %d scalar NaN payload/sign deviations tolerated\n", m.c_str(), n);
    }
    std::printf("%zu mnemonics, %d states, %d failed (%d mnemonics), %d tolerated NaN deviations\n", stats.size(), total, failed, mfail, tol);
    return failed ? 1 : 0;
}
