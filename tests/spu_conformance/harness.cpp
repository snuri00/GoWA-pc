#include "spu/context.h"
#include "ppu/ops.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <xmmintrin.h>

namespace {

class NullHost : public spu::Host {
public:
    uint32_t read_channel(uint32_t) override { return 0; }
    void write_channel(uint32_t, uint32_t) override {}
    uint32_t channel_count(uint32_t) override { return 0; }
    bool stop_and_signal(uint32_t) override { return false; }
    void halt(uint32_t, uint32_t) override {}
};

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

spu::v128 load_be(const uint8_t* p) {
    spu::v128 v;
    for (int i = 0; i < 16; i++) v.u8[15 - i] = p[i];
    return v;
}

std::string hex(const spu::v128& v) {
    char b[40];
    std::snprintf(b, sizeof b, "%08x%08x%08x%08x", v.u32[3], v.u32[2], v.u32[1], v.u32[0]);
    return b;
}

bool same(const spu::v128& a, const spu::v128& b) { return std::memcmp(&a, &b, 16) == 0; }

}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: spu_conformance <dir> <rpcs3 output>\n");
        return 2;
    }
    std::string dir = argv[1];
    std::ifstream list(dir + "/tests.txt");
    uint32_t states = 0;
    list >> states;
    std::vector<std::pair<std::string, uint32_t>> tests;
    std::string m;
    std::string w;
    while (list >> m >> w) tests.push_back({m, static_cast<uint32_t>(std::stoul(w, nullptr, 16))});
    std::vector<uint8_t> in = read_file(dir + "/inputs.bin");
    std::vector<uint8_t> want = read_file(argv[2]);
    const size_t state_size = 0x800;
    if (in.size() != tests.size() * states * state_size || want.size() != in.size()) {
        std::fprintf(stderr, "size mismatch: tests %zu states %u inputs %zu expected %zu\n", tests.size(), states, in.size(), want.size());
        return 2;
    }
    _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x6000u | 0x8040u);
    std::vector<uint8_t> ls(spu::LS_SIZE);
    NullHost host;
    std::map<std::string, std::pair<int, int>> stats;
    int shown = 0;
    for (size_t k = 0; k < tests.size(); k++) {
        auto& [name, word] = tests[k];
        for (uint32_t s = 0; s < states; s++) {
            size_t off = (k * states + s) * state_size;
            spu::Context ctx{};
            ctx.ls = ls.data();
            for (int r = 0; r < 128; r++) ctx.gpr[r] = load_be(in.data() + off + r * 16);
            ls[0] = static_cast<uint8_t>(word >> 24);
            ls[1] = static_cast<uint8_t>(word >> 16);
            ls[2] = static_cast<uint8_t>(word >> 8);
            ls[3] = static_cast<uint8_t>(word);
            ctx.pc = 0;
            spu::step(ctx, host);
            bool ok = true;
            for (int r = 0; r < 127; r++) {
                spu::v128 expect = load_be(want.data() + off + r * 16);
                if (!same(expect, ctx.gpr[r])) {
                    ok = false;
                    if (shown < 300) {
                        spu::v128 ra = load_be(in.data() + off + ((word >> 7) & 0x7F) * 16);
                        spu::v128 rb = load_be(in.data() + off + ((word >> 14) & 0x7F) * 16);
                        spu::v128 rc = load_be(in.data() + off + (word & 0x7F) * 16);
                        std::printf("MISMATCH %-10s %08x state %u: r%d got=%s want=%s ra=%s rb=%s rc/rt=%s\n", name.c_str(), word, s, r, hex(ctx.gpr[r]).c_str(),
                                    hex(expect).c_str(), hex(ra).c_str(), hex(rb).c_str(), hex(rc).c_str());
                        shown++;
                    }
                }
            }
            auto& st = stats[name];
            st.second++;
            if (!ok) st.first++;
        }
    }
    int failed = 0, total = 0;
    for (auto& [name, st] : stats) {
        total += st.second;
        if (st.first) {
            failed += st.first;
            std::printf("  %-10s %d/%d failed\n", name.c_str(), st.first, st.second);
        }
    }
    std::printf("%zu mnemonics, %d states, %d mismatching\n", stats.size(), total, failed);
    return failed ? 1 : 0;
}
