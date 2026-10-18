#include "edge_vcomp.h"
#include "natreg.h"
#include "unit.h"

#include <bit>
#include <chrono>
#include <random>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace {

uint32_t rbe32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

struct Cb {
    std::string name;
    uint32_t ea, size;
};

const std::vector<Cb>& compress_callbacks() {
    static std::vector<Cb> v;
    if (!v.empty()) return v;
    const std::vector<uint8_t>& e = harness::elf();
    std::map<uint32_t, std::string> names;
    for (int i = 0; i < 32; i++) {
        std::string n = "smCompressVertices_" + std::to_string(i);
        uint32_t h = 0x811c9dc5u;
        for (unsigned char ch : n) h = (h * 0x01000193u) ^ ch;
        names[h] = n;
    }
    uint32_t off = 0x754e00 - 0x10000;
    uint32_t cnt = (e[off] << 8) | e[off + 1];
    uint32_t doff = (e[off + 2] << 8) | e[off + 3];
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t* p = e.data() + off + 4 + 8 * i;
        uint32_t h = rbe32(p);
        if (!names.count(h)) continue;
        v.push_back({names[h], 0x754e00 + doff + 16 * ((p[4] << 8) | p[5]), uint32_t((p[6] << 8) | p[7])});
    }
    return v;
}

int identify(const uint8_t* ls, uint32_t addr) {
    const auto& cbs = compress_callbacks();
    const std::vector<uint8_t>& e = harness::elf();
    for (size_t k = 0; k < cbs.size(); k++) {
        if (addr + cbs[k].size > 0x40000) continue;
        if (std::memcmp(ls + addr, e.data() + cbs[k].ea - 0x10000, cbs[k].size) == 0) return int(k);
    }
    return -1;
}

int overlay_id(const std::string& name) { return std::atoi(name.c_str() + std::strlen("smCompressVertices_")); }


int cmd_vcstat(const std::vector<std::string>& args) {
    std::map<std::string, int> count;
    bool verbose = args.size() > 1 && args[1] == "-v";
    edgeunit::for_each_call(harness::cap_list(args, verbose ? 2 : 1), 0xd158, [&](edgeunit::Hit& h) {
        const uint8_t* b = h.before.ls.data();
        uint32_t ctx = h.before.regs.gpr[3].u32[3] & 0x3fff0;
        uint32_t cb = rbe32(b + ctx + 140);
        int k = cb ? identify(b, cb & 0x3fffc) : -1;
        std::string nm = k >= 0 ? compress_callbacks()[k].name : (cb ? "unknown" : "none");
        count[nm]++;
        if (verbose) {
            std::printf("%s job %d insns %llu %s cb %05x nv %u fmt %u r3out %05x ctx0 ", h.cap.substr(h.cap.size() - 9).c_str(), h.job, (unsigned long long)h.fn_insns, nm.c_str(), cb, (b[ctx + 8] << 8) | b[ctx + 9], b[ctx + 4], h.after.regs.gpr[3].u32[3]);
            for (int i = 0; i < 16; i++) std::printf("%02x", b[ctx + i]);
            std::printf(" c80 ");
            for (int i = 80; i < 144; i += 4) std::printf("%08x ", rbe32(b + ctx + i));
            std::printf("c352 ");
            for (int i = 352; i < 368; i++) std::printf("%02x", b[ctx + i]);
            std::printf(" diffs");
            const uint8_t* a = h.after.ls.data();
            uint32_t lo = h.sp - 0x10000;
            for (uint32_t x = 0; x < 0x40000;) {
                if ((x >= lo && x < h.sp) || a[x] == b[x]) {
                    x++;
                    continue;
                }
                uint32_t y = x;
                while (y < 0x40000 && (a[y] != b[y] || (y + 1 < 0x40000 && a[y + 1] != b[y + 1]) || (y + 2 < 0x40000 && a[y + 2] != b[y + 2]))) y++;
                std::printf(" [%05x,%05x)", x, y);
                x = y;
            }
            std::printf("\n");
        }
        return true;
    }, harness::edge_loaded);
    for (auto& [k, n] : count) std::printf("%s %d\n", k.c_str(), n);
    return 0;
}

CommandReg reg_vcstat("vcstat", cmd_vcstat);

}

namespace {

int cmd_vcbench(const std::vector<std::string>& args) {
    std::map<int, std::pair<double, uint64_t>> per;
    std::map<int, uint64_t> spu, verts, calls;
    edgeunit::for_each_call(harness::cap_list(args, 1), 0xd158, [&](edgeunit::Hit& h) {
        std::vector<uint8_t> ls = h.before.ls;
        ls.resize(0x40010);
        uint32_t ctx = h.before.regs.gpr[3].u32[3] & 0x3fff0;
        uint32_t cb = rbe32(ls.data() + ctx + 140) & 0x3fffc;
        int k = cb ? identify(ls.data(), cb) : -1;
        int id = k >= 0 ? overlay_id(compress_callbacks()[k].name) : -1;
        uint8_t save[16];
        std::memcpy(save, ls.data() + ctx + 80, 16);
        uint32_t r3[4];
        const int reps = 50;
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < reps; i++) {
            std::memcpy(ls.data() + ctx + 80, save, 16);
            edge::compress_vertexes(ls.data(), ctx, id, r3, nullptr);
        }
        double us = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 1e6 / reps;
        per[id].first += us;
        spu[id] += h.fn_insns;
        verts[id] += (ls[ctx + 8] << 8) | ls[ctx + 9];
        calls[id]++;
        return true;
    }, harness::edge_loaded);
    for (auto& [id, v] : per)
        std::printf("overlay %d calls %llu verts/call %.0f spu-insns/call %.0f native-us/call %.3f ns/vertex %.2f\n", id, (unsigned long long)calls[id], double(verts[id]) / calls[id], double(spu[id]) / calls[id], v.first / calls[id], v.first * 1000 / verts[id]);
    return 0;
}

CommandReg reg_vcbench("vcbench", cmd_vcbench);

}

namespace {

uint32_t fuzz_word(std::mt19937& g) {
    uint32_t k = g() % 12;
    float f;
    switch (k) {
    case 0: f = std::uniform_real_distribution<float>(-1.2f, 1.2f)(g); return std::bit_cast<uint32_t>(f);
    case 1: f = std::uniform_real_distribution<float>(-70000.0f, 70000.0f)(g); return std::bit_cast<uint32_t>(f);
    case 2: return (g() & 0x80000000u) | 0x3f800000u;
    case 3: return (g() & 0x80000000u) | 0x7f800000u | (g() & 0x7fffff);
    case 4: return (g() & 0x80000000u) | (g() & 0x7fffff);
    case 5: return g();
    case 6: return (g() & 0x80000000u) | (0x37800000u + (g() % 0x01800000u));
    case 7: return (g() & 0x80000000u) | (0x47700000u + (g() % 0x00200000u));
    case 8: return g() & 0x80000000u;
    case 9: return (g() & 0x80000000u) | (0x46ffff00u + (g() % 512));
    default: f = std::uniform_real_distribution<float>(-1.0f, 1.0f)(g); return std::bit_cast<uint32_t>(f);
    }
}

int cmd_vcfuzz(const std::vector<std::string>& args) {
    int per = args.size() > 1 ? std::stoi(args[1]) : 20;
    std::mt19937 g(args.size() > 2 ? std::stoul(args[2]) : 1);
    std::map<int, int> done, bad;
    edgeunit::for_each_call(harness::cap_list(args, 3), 0xd158, [&](edgeunit::Hit& h) {
        uint32_t ctx = h.before.regs.gpr[3].u32[3] & 0x3fff0;
        uint8_t* b = h.before.ls.data();
        uint32_t cb = rbe32(b + ctx + 140) & 0x3fffc;
        int k = cb ? identify(b, cb) : -1;
        if (k < 0) return true;
        int id = overlay_id(compress_callbacks()[k].name);
        if (done[id] >= per) return true;
        done[id]++;
        uint32_t nv = (b[ctx + 8] << 8) | b[ctx + 9];
        uint32_t nnv = 1 + g() % nv;
        b[ctx + 8] = uint8_t(nnv >> 8);
        b[ctx + 9] = uint8_t(nnv);
        for (int s = 0; s < 16 && b[ctx + 352 + s]; s++) {
            uint32_t t = rbe32(b + ctx + 16 + 4 * s) & 0x3fff0;
            for (uint32_t w = 0; w < 4 * (nv + 1) && t + 4 * w + 4 <= 0x40000; w++) {
                uint32_t x = fuzz_word(g);
                b[t + 4 * w] = uint8_t(x >> 24);
                b[t + 4 * w + 1] = uint8_t(x >> 16);
                b[t + 4 * w + 2] = uint8_t(x >> 8);
                b[t + 4 * w + 3] = uint8_t(x);
            }
        }
        edgeunit::Snapshot iso = h.before;
        edgeunit::IsoResult r = edgeunit::run_isolated(iso, h.ret, h.sp);
        edgeunit::Snapshot ns = h.before;
        ns.ls.resize(0x40010);
        ns.regs.ls = ns.ls.data();
        edge::Outputs outs;
        bool ok = natives()[0xd158](ns.ls.data(), ns.regs, outs);
        edgeunit::LsDiff d = edgeunit::diff_ls(ns.ls.data(), iso.ls.data(), h.sp);
        bool r3 = std::memcmp(&ns.regs.gpr[3], &iso.regs.gpr[3], 16) == 0;
        if (!r.ok || !ok || d.bytes || !r3) {
            if (bad[id]++ < 5) std::printf("overlay %d nv %u: iso %d native %d diff %zu [%05x..%05x] r3 %d\n", id, nnv, r.ok, ok, d.bytes, d.first, d.last, r3);
        }
        return true;
    }, harness::edge_loaded);
    for (auto& [id, n] : done) std::printf("overlay %d fuzzed %d bad %d\n", id, n, bad[id]);
    return 0;
}

CommandReg reg_vcfuzz("vcfuzz", cmd_vcfuzz);

}
