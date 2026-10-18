#include "edge_vdecomp.h"
#include "natreg.h"
#include "unit.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace {

struct Cb {
    std::string name;
    uint32_t ea, size;
};

std::vector<Cb>& cbs() {
    static std::vector<Cb> v;
    if (!v.empty()) return v;
    const auto& e = harness::elf();
    auto be = [&](uint32_t o) { return (uint32_t(e[o]) << 24) | (uint32_t(e[o + 1]) << 16) | (uint32_t(e[o + 2]) << 8) | e[o + 3]; };
    std::map<uint32_t, std::string> names;
    const char* pre[] = {"smDecompressVertices_", "smCompressVertices_", "smSetVertexDataArrays_"};
    for (auto p : pre)
        for (int i = 0; i < 32; i++) {
            std::string s = std::string(p) + std::to_string(i);
            uint32_t h = 0x811c9dc5u;
            for (unsigned char ch : s) h = (h * 0x01000193u) ^ ch;
            names[h] = s;
        }
    uint32_t off = 0x754e00 - 0x10000;
    uint32_t cnt = (e[off] << 8) | e[off + 1];
    uint32_t doff = (e[off + 2] << 8) | e[off + 3];
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t o = off + 4 + 8 * i;
        uint32_t h = be(o);
        Cb c;
        c.name = names.count(h) ? names[h] : "?";
        c.ea = 0x754e00 + doff + 16 * ((e[o + 4] << 8) | e[o + 5]);
        c.size = (e[o + 6] << 8) | e[o + 7];
        v.push_back(c);
    }
    return v;
}

int identify(const uint8_t* ls, uint32_t entry) {
    const auto& e = harness::elf();
    int k = 0;
    for (const Cb& c : cbs()) {
        if (entry + c.size <= 0x40000 && std::memcmp(ls + entry, e.data() + c.ea - 0x10000, c.size) == 0) return k;
        k++;
    }
    return -1;
}

int cmd_vstats(const std::vector<std::string>& args) {
    std::map<std::string, std::pair<int, uint64_t>> agg;
    bool verbose = args.size() > 1 && args[1] == "v";
    edgeunit::for_each_call(harness::cap_list(args, verbose ? 2 : 1), 0xdbc8, [&](edgeunit::Hit& h) {
        const spu::Context& c = h.before.regs;
        uint32_t r8 = c.gpr[8].u32[3];
        uint32_t ctx = c.gpr[3].u32[3];
        int k = r8 ? identify(h.before.ls.data(), r8) : -2;
        std::string nm = k >= 0 ? cbs()[k].name : (k == -2 ? "null" : "unknown");
        agg[nm].first++;
        agg[nm].second += h.fn_insns;
        if (verbose) {
            std::printf("%s job %d %s insns %llu r3 %05x r4 %05x r5 %05x r6 %05x r7 %05x r8 %05x nv %u cfg ", h.cap.substr(h.cap.size() - 9).c_str(), h.job, nm.c_str(), (unsigned long long)h.fn_insns, ctx, c.gpr[4].u32[3], c.gpr[5].u32[3], c.gpr[6].u32[3], c.gpr[7].u32[3], r8,
                        (h.before.ls[ctx + 8] << 8) | h.before.ls[ctx + 9]);
            for (int i = 0; i < 16; i++) std::printf("%02x", h.before.ls[ctx + i]);
            std::printf("\n");
        }
        return true;
    }, harness::edge_loaded);
    for (auto& [k, v] : agg) std::printf("%-26s hits %5d insns %10llu (%.0f/call)\n", k.c_str(), v.first, (unsigned long long)v.second, double(v.second) / v.first);
    return 0;
}

CommandReg reg_vstats("vstats", cmd_vstats);


int cmd_vunit(const std::vector<std::string>& args) {
    struct St {
        int hits = 0, exact = 0, declined = 0, bad = 0;
        uint64_t insns = 0;
        double sec = 0;
    };
    std::map<int, St> st;
    int shown = 0;
    edgeunit::for_each_call(harness::cap_list(args, 1), 0xdbc8, [&](edgeunit::Hit& h) {
        const spu::Context& c0 = h.before.regs;
        uint32_t r8 = c0.gpr[8].u32[3] & 0x3fffc;
        int ov = r8 ? edge::vdecomp_identify(h.before.ls.data(), r8) : -2;
        St& s = st[ov];
        s.hits++;
        s.insns += h.fn_insns;
        edgeunit::Snapshot ns = h.before;
        ns.ls.resize(0x40010);
        ns.regs.ls = ns.ls.data();
        edge::Outputs outs;
        bool ok = false;
        double best = 1e9;
        for (int rep = 0; rep < 4; rep++) {
            std::memcpy(ns.ls.data(), h.before.ls.data(), 0x40000);
            ns.regs = h.before.regs;
            ns.regs.ls = ns.ls.data();
            outs = edge::Outputs{};
            auto t0 = std::chrono::steady_clock::now();
            ok = natives()[0xdbc8](ns.ls.data(), ns.regs, outs);
            best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        }
        s.sec += best;
        if (!ok) {
            s.declined++;
            return true;
        }
        edgeunit::LsDiff d = edgeunit::diff_ls(ns.ls.data(), h.after.ls.data(), h.sp, 0);
        bool r3 = std::memcmp(&ns.regs.gpr[3], &h.after.regs.gpr[3], 16) == 0;
        if (!d.bytes && r3) s.exact++;
        else {
            s.bad++;
            if (shown++ < 6) {
                std::printf("  ov %d %s job %d: ls diff %zu [%05x..%05x] r3 %s\n", ov, h.cap.c_str(), h.job, d.bytes, d.first, d.last, r3 ? "ok" : "DIFF");
                int n = 0;
                for (uint32_t a = 0; a < 0x40000 && n < 8; a++)
                    if (ns.ls[a] != h.after.ls[a] && !(a >= h.sp - 0x10000 && a < h.sp)) {
                        std::printf("    %05x native %02x spu %02x before %02x\n", a, ns.ls[a], h.after.ls[a], h.before.ls[a]);
                        n++;
                    }
            }
        }
        return true;
    }, harness::edge_loaded);
    for (auto& [ov, s] : st) std::printf("overlay %3d hits %5d exact %5d bad %4d declined %4d spu %8.0f insns/call native %7.2f us/call\n", ov, s.hits, s.exact, s.bad, s.declined, double(s.insns) / s.hits, s.sec * 1e6 / s.hits);
    return 0;
}

CommandReg reg_vunit("vunit", cmd_vunit);

}
