#include "edge_ovl.h"
#include "edge_ovl_gen.h"
#include "natreg.h"
#include "unit.h"

#include <immintrin.h>
#include <intrin.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace {

class FailHost : public spu::Host {
public:
    bool failed = false;
    uint32_t read_channel(uint32_t) override {
        failed = true;
        return 0;
    }
    void write_channel(uint32_t, uint32_t) override { failed = true; }
    uint32_t channel_count(uint32_t) override {
        failed = true;
        return 0;
    }
    bool stop_and_signal(uint32_t) override {
        failed = true;
        return false;
    }
    void halt(uint32_t, uint32_t) override { failed = true; }
};

enum Kind { kDecomp, kComp, kSet };

struct Tmpl {
    Kind kind;
    edgeunit::Snapshot s;
};

uint32_t rbe(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
void wbe(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

uint32_t rand_float(std::mt19937& rng) {
    switch (rng() % 10) {
    case 0: return 0;
    case 1: return 0x80000000u;
    case 2: return (rng() & 0x80000000u) | 0x7f800000u | (rng() & 0x7fffff);
    case 3: return (rng() & 0x80000000u) | (rng() & 0x7fffff);
    case 4: return (rng() & 0x80000000u) | 0x3f800000u;
    case 5: return rng();
    default: {
        float f = float(int32_t(rng() % 4000) - 2000) / float(1 + rng() % 1000);
        uint32_t u;
        std::memcpy(&u, &f, 4);
        return u;
    }
    }
}

int name_id(const char* n, const char* prefix) {
    size_t l = std::strlen(prefix);
    if (std::strncmp(n, prefix, l) != 0) return -1;
    return std::atoi(n + l);
}

bool prerun(spu::Context& c, uint32_t target, uint64_t max) {
    FailHost h;
    for (uint64_t i = 0; i < max; i++) {
        if (c.pc == target) return true;
        spu::step(c, h);
        if (h.failed || c.halted) return false;
    }
    return c.pc == target;
}

int cmd_ovlfuzz(const std::vector<std::string>& args) {
    int per = args.size() > 1 ? std::atoi(args[1].c_str()) : 20;
    uint32_t seed = args.size() > 2 ? uint32_t(std::strtoul(args[2].c_str(), nullptr, 0)) : 1;
    std::string only = args.size() > 3 ? args[3] : "";
    std::vector<std::string> caps = harness::cap_list(args, 4);
    _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x6000u | 0x8040u);
    std::vector<Tmpl> tm;
    const std::pair<uint32_t, Kind> fns[] = {{0xdbc8, kDecomp}, {0xd158, kComp}, {0x5d68, kSet}};
    for (auto [fn, kind] : fns) {
        int got = 0;
        edgeunit::for_each_call(caps, fn, [&](edgeunit::Hit& h) {
            if (h.job % 7 != 0 && got > 2) return true;
            tm.push_back({kind, h.before});
            return ++got < 12;
        }, harness::edge_loaded);
    }
    std::printf("templates: %zu\n", tm.size());
    std::mt19937 rng(seed);
    FailHost host;
    std::vector<uint8_t> ls_a(spu::LS_SIZE + 16), ls_b(spu::LS_SIZE + 16);
    int total_bad = 0;
    for (size_t ei = 0; ei < edge::ovl::kEntryCount; ei++) {
        const edge::ovl::Entry& e = edge::ovl::kEntries[ei];
        if (!only.empty() && only != "all" && std::string(e.name).find(only) == std::string::npos) continue;
        int kinds_ok[3] = {-1, -1, -1};
        int did = name_id(e.name, "smDecompressVertices_");
        int cid = name_id(e.name, "smCompressVertices_");
        int sid = name_id(e.name, "smSetVertexDataArrays_");
        if (did >= 0) kinds_ok[0] = did;
        else if (cid >= 0) kinds_ok[1] = cid;
        else if (sid >= 0) kinds_ok[2] = sid;
        else kinds_ok[0] = kinds_ok[1] = kinds_ok[2] = 0;
        int cases = 0, reached = 0, bad = 0, partial = 0;
        uint64_t nat_cyc = 0, int_cyc = 0, insns = 0;
        for (int k = 0; k < per * 4 && cases < per; k++) {
            const Tmpl& t = tm[rng() % tm.size()];
            if (kinds_ok[t.kind] < 0) continue;
            cases++;
            std::memcpy(ls_a.data(), t.s.ls.data(), spu::LS_SIZE);
            spu::Context c = t.s.regs;
            c.ls = ls_a.data();
            c.line_epoch = nullptr;
            c.halted = false;
            uint32_t B = 0x38000 + 16 * (rng() % 64);
            std::memcpy(ls_a.data() + B, e.code, e.size);
            uint32_t ctx = c.gpr[3].u32[3] & 0x3fff0;
            uint8_t* L = ls_a.data();
            uint32_t nv = (L[ctx + 8] << 8) | L[ctx + 9];
            bool mutate = rng() % 4 != 0;
            if (t.kind == kDecomp) {
                c.gpr[8].u32[3] = B;
                L[ctx + 2] = uint8_t(kinds_ok[0]);
                c.gpr[6].u32[3] = (c.gpr[6].u32[3] & ~0xffu) | uint32_t(kinds_ok[0]);
                if (mutate) {
                    uint32_t src = c.gpr[4].u32[3] & 0x3fff0;
                    uint32_t len = (std::min)(uint32_t(64 * (nv + 2)), 0x38000u - src);
                    for (uint32_t i = 0; i + 4 <= len; i += 4) wbe(L + src + i, rng() % 3 ? uint32_t(rng()) : rand_float(rng));
                    uint32_t d = c.gpr[5].u32[3] & 0x3fff0;
                    for (int i = 0; i < 3; i++) wbe(L + d + 4 * i, rng() % 2 ? 0 : uint32_t(rng() % 4096) - 2048);
                    for (int i = 0; i < 3; i++) {
                        uint32_t ib = rng() % 17, fb = rng() % (17 - ib);
                        if (rng() % 3 == 0) ib = rng() % 33, fb = rng() % 33;
                        L[d + 16 + 2 * i] = uint8_t(ib);
                        L[d + 17 + 2 * i] = uint8_t(fb);
                    }
                }
            } else if (t.kind == kComp) {
                wbe(L + ctx + 140, B);
                L[ctx + 4] = uint8_t(kinds_ok[1]);
                if (mutate) {
                    for (int s = 0; s < 8; s++) {
                        uint32_t tp = rbe(L + ctx + 16 + 4 * s) & 0x3fff0;
                        if (tp < 0x11000 || tp >= 0x38000) continue;
                        uint32_t len = (std::min)(16 * (nv + 2), 0x38000u - tp);
                        for (uint32_t i = 0; i + 4 <= len; i += 4) wbe(L + tp + i, rand_float(rng));
                    }
                    if (rng() % 3 == 0) {
                        const uint8_t ids[] = {0, 1, 2, 3, 4, 27, 28, 29, 30, 31};
                        for (int s = 0; s < 8; s++) L[ctx + 352 + s] = ids[rng() % 10];
                    }
                }
            } else {
                c.gpr[8].u32[3] = B;
            }
            if (!prerun(c, B, 20000)) continue;
            reached++;
            std::memcpy(ls_b.data(), ls_a.data(), spu::LS_SIZE + 16);
            spu::Context n = c;
            n.ls = ls_b.data();
            uint64_t budget = rng() % 4 == 0 ? 1 + rng() % 3000 : (1ull << 40);
            uint64_t t0 = __rdtsc();
            bool ok = e.run(n, host, B, budget);
            nat_cyc += __rdtsc() - t0;
            if (!ok || host.failed) {
                bad++;
                std::printf("  %s case %d: native returned %d failed %d\n", e.name, cases, ok, host.failed);
                host.failed = false;
                continue;
            }
            uint64_t kk = n.executed;
            if (n.pc >= B && n.pc < B + e.size) partial++;
            FailHost ih;
            t0 = __rdtsc();
            for (uint64_t i = 0; i < kk && !ih.failed && !c.halted; i++) spu::step(c, ih);
            int_cyc += __rdtsc() - t0;
            insns += kk;
            bool same = !ih.failed && c.pc == n.pc && std::memcmp(c.gpr, n.gpr, sizeof c.gpr) == 0 && std::memcmp(ls_a.data(), ls_b.data(), spu::LS_SIZE) == 0;
            if (!same) {
                bad++;
                if (bad <= 3) {
                    std::printf("  %s case %d MISMATCH: insns %llu pc int %05x nat %05x fail %d", e.name, cases, (unsigned long long)kk, c.pc, n.pc, ih.failed);
                    for (int r = 0; r < 128; r++)
                        if (std::memcmp(&c.gpr[r], &n.gpr[r], 16)) std::printf(" r%d", r);
                    for (uint32_t a = 0; a < spu::LS_SIZE; a += 16)
                        if (std::memcmp(ls_a.data() + a, ls_b.data() + a, 16)) {
                            std::printf(" ls@%05x", a);
                            break;
                        }
                    std::printf("\n");
                }
            }
        }
        total_bad += bad;
        std::printf("%-26s cases %3d reached %3d partial %3d bad %d insns/case %6.0f cyc/insn native %.2f interp %.2f\n", e.name, cases, reached, partial, bad, reached ? double(insns) / reached : 0.0, insns ? double(nat_cyc) / insns : 0.0,
                    insns ? double(int_cyc) / insns : 0.0);
    }
    std::printf("total mismatches %d\n", total_bad);
    return total_bad ? 1 : 0;
}

CommandReg reg_ovlfuzz("ovlfuzz", cmd_ovlfuzz);

int cmd_vfuzz(const std::vector<std::string>& args) {
    int per = args.size() > 1 ? std::atoi(args[1].c_str()) : 50;
    uint32_t seed = args.size() > 2 ? uint32_t(std::strtoul(args[2].c_str(), nullptr, 0)) : 1;
    std::string only = args.size() > 3 ? args[3] : "smDecompressVertices_";
    std::vector<std::string> caps = harness::cap_list(args, 4);
    _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x6000u | 0x8040u);
    std::vector<edgeunit::Hit> tm;
    edgeunit::for_each_call(caps, 0xdbc8, [&](edgeunit::Hit& h) {
        if (h.job % 5 != 0 && tm.size() > 2) return true;
        tm.push_back(h);
        return tm.size() < 16;
    }, harness::edge_loaded);
    std::printf("templates: %zu\n", tm.size());
    NativeFn nat = natives()[0xdbc8];
    std::mt19937 rng(seed);
    std::vector<uint8_t> la(spu::LS_SIZE + 16), lb(spu::LS_SIZE + 16);
    int total_bad = 0;
    for (size_t ei = 0; ei < edge::ovl::kEntryCount; ei++) {
        const edge::ovl::Entry& e = edge::ovl::kEntries[ei];
        if (std::string(e.name).find(only) == std::string::npos) continue;
        if (only.back() == '_' && std::strncmp(e.name, "smDecompressVertices_", 21) != 0) continue;
        int id = name_id(e.name, "smDecompressVertices_");
        if (id < 0) continue;
        int cases = 0, declined = 0, bad = 0;
        for (int k = 0; k < per; k++) {
            const edgeunit::Hit& t = tm[rng() % tm.size()];
            std::memcpy(la.data(), t.before.ls.data(), spu::LS_SIZE);
            spu::Context c = t.before.regs;
            c.ls = la.data();
            c.line_epoch = nullptr;
            c.halted = false;
            uint32_t B = 0x38000 + 16 * (rng() % 64);
            std::memcpy(la.data() + B, e.code, e.size);
            uint8_t* L = la.data();
            uint32_t ctx = c.gpr[3].u32[3] & 0x3fff0;
            uint32_t nv = (L[ctx + 8] << 8) | L[ctx + 9];
            c.gpr[8].u32[3] = B;
            L[ctx + 2] = uint8_t(id);
            c.gpr[6].u32[3] = (c.gpr[6].u32[3] & ~0xffu) | uint32_t(id);
            uint32_t src = c.gpr[4].u32[3] & 0x3fff0;
            uint32_t len = (std::min)(uint32_t(32 * (nv + 2)), 0x38000u - src);
            if (rng() % 4) for (uint32_t i = 0; i + 4 <= len; i += 4) wbe(L + src + i, rng() % 3 ? uint32_t(rng()) : rand_float(rng));
            uint32_t d = c.gpr[5].u32[3] & 0x3fff0;
            if (rng() % 3 == 0) {
                for (int i = 0; i < 3; i++) wbe(L + d + 4 * i, uint32_t(rng() % 65536) - 32768);
                for (int i = 0; i < 3; i++) {
                    uint32_t ib = rng() % 17, fb = rng() % (17 - ib);
                    L[d + 16 + 2 * i] = uint8_t(ib);
                    L[d + 17 + 2 * i] = uint8_t(fb);
                }
            }
            if (rng() % 4 == 0) {
                uint32_t nn = 1 + rng() % (nv ? nv : 1);
                L[ctx + 8] = uint8_t(nn >> 8);
                L[ctx + 9] = uint8_t(nn);
            }
            cases++;
            std::memcpy(lb.data(), la.data(), spu::LS_SIZE + 16);
            spu::Context n = c;
            n.ls = lb.data();
            edge::Outputs outs;
            if (!nat(lb.data(), n, outs)) {
                declined++;
                continue;
            }
            edgeunit::Snapshot s;
            s.ls.assign(la.begin(), la.begin() + spu::LS_SIZE);
            s.regs = c;
            edgeunit::IsoResult ir = edgeunit::run_isolated(s, t.ret, t.sp);
            if (!ir.ok) {
                std::printf("  %s case %d: interpreter %s\n", e.name, cases, ir.error.c_str());
                bad++;
                continue;
            }
            edgeunit::LsDiff df = edgeunit::diff_ls(s.ls.data(), lb.data(), t.sp, 0);
            bool r3 = std::memcmp(&s.regs.gpr[3], &n.gpr[3], 16) == 0;
            if (df.bytes || !r3) {
                bad++;
                if (bad <= 3) std::printf("  %s case %d MISMATCH: ls bytes %zu first %05x last %05x r3 %d nv %u\n", e.name, cases, df.bytes, df.first, df.last, r3, nv);
            }
        }
        total_bad += bad;
        std::printf("%-26s cases %3d native %3d declined %3d bad %d\n", e.name, cases, cases - declined, declined, bad);
    }
    std::printf("total mismatches %d\n", total_bad);
    return total_bad ? 1 : 0;
}

CommandReg reg_vfuzz("vfuzz", cmd_vfuzz);

}
