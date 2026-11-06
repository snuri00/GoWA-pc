#include "edge_softbody.h"
#include "edge_ovl.h"
#include "spu_float.h"
#include "edge_vdecomp.h"
#include "natreg.h"
#include "unit.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

struct DeclineCap {
    uint32_t native = 0;
    uint64_t key = 0;
    uint32_t pc = 0;
    edgeunit::Snapshot s;
};

bool load_decline(const std::string& path, DeclineCap& d) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint32_t hdr[6];
    bool ok = std::fread(hdr, sizeof hdr, 1, f) == 1 && hdr[0] == 0x4c434445u && hdr[1] == 1;
    if (ok) {
        d.native = hdr[2];
        d.key = uint64_t(hdr[3]) | (uint64_t(hdr[4]) << 32);
        d.pc = hdr[5];
        ok = std::fread(d.s.regs.gpr, sizeof d.s.regs.gpr, 1, f) == 1;
        d.s.ls.assign(spu::LS_SIZE + 16, 0);
        ok = ok && std::fread(d.s.ls.data(), 1, spu::LS_SIZE, f) == spu::LS_SIZE;
    }
    std::fclose(f);
    d.s.regs.pc = d.pc;
    d.s.regs.ls = d.s.ls.data();
    return ok;
}

int cmd_decl(const std::vector<std::string>& args) {
    if (args.size() < 3) {
        std::printf("usage: decl adapter|softbody <capture files...>\n");
        return 2;
    }
    bool raw_sb = args[1] == "softbody";
    int ran = 0, declined = 0, bad = 0, ref_failed = 0;
    for (size_t i = 2; i < args.size(); i++) {
        DeclineCap d;
        if (!load_decline(args[i], d)) {
            std::printf("  %s: unreadable\n", args[i].c_str());
            continue;
        }
        uint32_t ret = d.s.regs.gpr[0].u32[3] & 0x3fffc, sp = d.s.regs.gpr[1].u32[3];
        edgeunit::Snapshot ref = d.s;
        edgeunit::IsoResult r = edgeunit::run_isolated(ref, ret, sp);
        if (!r.ok) {
            ref_failed++;
            std::printf("  %s: reference run failed (%s)\n", args[i].c_str(), r.error.c_str());
            continue;
        }
        if (const char* dump = std::getenv("DECL_DUMPREF")) {
            if (FILE* f = std::fopen((std::string(dump) + "." + std::to_string(i)).c_str(), "wb")) {
                std::fwrite(ref.ls.data(), 1, spu::LS_SIZE, f);
                std::fwrite(ref.regs.gpr, sizeof ref.regs.gpr, 1, f);
                std::fclose(f);
            }
        }
        edgeunit::Snapshot ns = d.s;
        ns.regs.ls = ns.ls.data();
        bool ok;
        if (raw_sb) {
            ok = edge::soft_body(ns.ls.data(), ns.regs.gpr);
        } else {
            NativeFn nat = natives().count(d.pc) ? natives()[d.pc] : nullptr;
            edge::Outputs outs;
            ok = nat && nat(ns.ls.data(), ns.regs, outs);
        }
        if (!ok) {
            declined++;
            std::printf("  %s: native declined (%llu SPU insns)\n", args[i].c_str(), static_cast<unsigned long long>(r.insns));
            continue;
        }
        ran++;
        edgeunit::LsDiff diff = edgeunit::diff_ls(ns.ls.data(), ref.ls.data(), sp, std::getenv("DECL_STACK") ? 0 : 0x1000);
        bool r3 = std::memcmp(&ns.regs.gpr[3], &ref.regs.gpr[3], 16) == 0;
        if (diff.bytes || !r3) {
            bad++;
            std::printf("  %s: MISMATCH ls %zu bytes [%05x..%05x] r3 %s (%llu SPU insns)\n", args[i].c_str(), diff.bytes, diff.first, diff.last, r3 ? "ok" : "DIFF", static_cast<unsigned long long>(r.insns));
            if (std::getenv("DECL_VERBOSE"))
                for (uint32_t a = diff.first & ~15u; a <= diff.last; a += 16) {
                    std::printf("    %05x orig", a);
                    for (int b = 0; b < 16; b++) std::printf(" %02x", d.s.ls[a + b]);
                    std::printf("\n          ref ");
                    for (int b = 0; b < 16; b++) std::printf(" %02x", ref.ls[a + b]);
                    std::printf("\n          nat ");
                    for (int b = 0; b < 16; b++) std::printf(" %02x", ns.ls[a + b]);
                    std::printf("\n");
                }
        } else {
            std::printf("  %s: ok (%llu SPU insns)\n", args[i].c_str(), static_cast<unsigned long long>(r.insns));
        }
    }
    std::printf("decl: %d ran, %d declined, %d mismatching, %d reference failures\n", ran, declined, bad, ref_failed);
    return bad ? 1 : 0;
}

CommandReg reg_decl("decl", cmd_decl);


uint32_t be32(const uint8_t* p) { return spuf::ld32(p); }

uint32_t e_s16(uint32_t x) { return uint32_t(spuf::cflts(spuf::bits(spuf::fma(spuf::fl(x), 32767.5f, -0.5f)), 157)) >> 16; }

uint32_t e_f16(uint32_t b) {
    uint32_t m = b & 0x7fffffffu;
    if (m < 0x38000000u) return 0;
    uint32_t h = m >= 0x47800000u ? 0x7bffu : ((m - 0x38000000u) >> 13) & 0x7fffu;
    return h | ((b >> 16) & 0x8000u);
}

uint32_t e_p111110(uint32_t x, uint32_t y, uint32_t z) {
    uint32_t X = uint32_t(spuf::cflts(spuf::bits(spuf::fma(spuf::fl(x), spuf::fl(0x3f7fe000), spuf::fl(0xba000000))), 142));
    uint32_t Y = uint32_t(spuf::cflts(spuf::bits(spuf::fma(spuf::fl(y), spuf::fl(0x3f7fe000), spuf::fl(0xba000000))), 142));
    uint32_t Z = uint32_t(spuf::cflts(spuf::bits(spuf::fma(spuf::fl(z), spuf::fl(0x3f7fc000), spuf::fl(0xba800000))), 142));
    return (Z & 0xffc00000u) | ((Y >> 21) << 11) | (X >> 21);
}

struct EncDef {
    const char* name;
    uint32_t size;
};

const EncDef kEncs[] = {{"F32X3", 12}, {"F32X4", 16}, {"S16X4", 8}, {"S16X3", 6}, {"P111110", 4}, {"F16X3", 6}, {"F16X4", 8}, {"U8X4_256", 4}, {"U8X4_255R", 4}};

uint32_t u8_256(uint32_t x) {
    float f = spuf::fl(x);
    if (!(f > 0.0f)) return 0;
    float s = f * 256.0f;
    return s >= 255.0f ? 255u : uint32_t(s);
}

uint32_t u8_255r(uint32_t x) { return uint32_t(spuf::cfltu(spuf::bits(spuf::fma(spuf::fl(x), spuf::fl(0x437f0000u), spuf::fl(0x3f000000u))), 149)) >> 24; }

void encode(int e, const uint8_t* src, uint8_t* o) {
    uint32_t x = be32(src), y = be32(src + 4), z = be32(src + 8), w = be32(src + 12);
    switch (e) {
    case 0: std::memcpy(o, src, 12); break;
    case 1: std::memcpy(o, src, 16); break;
    case 2: spuf::st16(o, e_s16(x)); spuf::st16(o + 2, e_s16(y)); spuf::st16(o + 4, e_s16(z)); spuf::st16(o + 6, e_s16(w)); break;
    case 3: spuf::st16(o, e_s16(x)); spuf::st16(o + 2, e_s16(y)); spuf::st16(o + 4, e_s16(z)); break;
    case 4: spuf::st32(o, e_p111110(x, y, z)); break;
    case 5: spuf::st16(o, e_f16(x)); spuf::st16(o + 2, e_f16(y)); spuf::st16(o + 4, e_f16(z)); break;
    case 6: spuf::st16(o, e_f16(x)); spuf::st16(o + 2, e_f16(y)); spuf::st16(o + 4, e_f16(z)); spuf::st16(o + 6, e_f16(w)); break;
    case 7: o[0] = uint8_t(u8_256(x)); o[1] = uint8_t(u8_256(y)); o[2] = uint8_t(u8_256(z)); o[3] = uint8_t(u8_256(w)); break;
    case 8: o[0] = uint8_t(u8_255r(x)); o[1] = uint8_t(u8_255r(y)); o[2] = uint8_t(u8_255r(z)); o[3] = uint8_t(u8_255r(w)); break;
    }
}

int cmd_vcfmt(const std::vector<std::string>& args) {
    for (size_t i = 1; i < args.size(); i++) {
        DeclineCap d;
        if (!load_decline(args[i], d) || d.native != 9) continue;
        uint32_t ret = d.s.regs.gpr[0].u32[3] & 0x3fffc, sp = d.s.regs.gpr[1].u32[3];
        edgeunit::Snapshot ref = d.s;
        if (!edgeunit::run_isolated(ref, ret, sp).ok) {
            std::printf("%s: reference failed\n", args[i].c_str());
            continue;
        }
        const uint8_t* ls = d.s.ls.data();
        const uint8_t* rl = ref.ls.data();
        uint32_t ctx = d.s.regs.gpr[3].u32[3] & 0x3fff0;
        uint32_t nv = spuf::ld16(ls + ctx + 8);
        uint32_t out = be32(ls + ctx + 88) & 0x3ffff;
        uint32_t p = be32(ls + ctx + 116);
        uint32_t rec = p ? ls[(p + 1) & 0x3ffff] : 0;
        uint32_t cb = be32(ls + ctx + 140) & 0x3fffc;
        const edge::ovl::Entry* ent = edge::ovl::find(ls, cb);
        std::printf("%s: overlay %05x %s, nv %u rec %u out %05x, attrs", args[i].c_str(), cb, ent ? edge::ovl::entry_name(size_t(edge::ovl::index_of(ent))) : "?", nv, rec, out);
        for (int k = 0; k < 16; k++)
            if (ls[ctx + 352 + k]) std::printf(" %u@%05x", ls[ctx + 352 + k], be32(ls + ctx + 16 + 4 * k));
        std::printf("\n  fields:");
        uint32_t off = 0;
        while (off < rec) {
            bool found = false;
            for (int k = 0; k < 16 && !found; k++) {
                uint8_t attr = ls[ctx + 352 + k];
                if (!attr) continue;
                uint32_t tab = be32(ls + ctx + 16 + 4 * k);
                for (int e = 0; e < 9 && !found; e++) {
                    if (e == 7) continue;
                    if (off + kEncs[e].size > rec) continue;
                    bool all = true;
                    for (uint32_t v = 0; v < nv && all; v++) {
                        uint8_t o[16];
                        encode(e, ls + ((tab + 16 * v) & 0x3fff0), o);
                        all = std::memcmp(o, rl + ((out + rec * v + off) & 0x3ffff), kEncs[e].size) == 0;
                    }
                    if (all) {
                        std::printf(" {%u, %s}", attr, kEncs[e].name);
                        off += kEncs[e].size;
                        found = true;
                    }
                }
            }
            if (!found) {
                std::printf(" ?? at +%u, bytes", off);
                for (uint32_t b = off; b < rec && b < off + 16; b++) std::printf(" %02x", rl[(out + b) & 0x3ffff]);
                break;
            }
        }
        std::printf("\n");
    }
    return 0;
}

CommandReg reg_vcfmt("vcfmt", cmd_vcfmt);

}

namespace {

int cmd_vcsig(const std::vector<std::string>& args) {
    std::set<std::string> seen;
    for (size_t i = 1; i < args.size(); i++) {
        DeclineCap d;
        if (!load_decline(args[i], d) || (d.native != 9 && d.native != 1)) continue;
        const uint8_t* ls = d.s.ls.data();
        uint32_t ptr;
        if (d.native == 9) {
            uint32_t ctx = d.s.regs.gpr[3].u32[3] & 0x3fff0;
            ptr = be32(ls + ctx + 140) & 0x3fffc;
        } else {
            ptr = d.s.regs.gpr[8].u32[3] & 0x3fffc;
        }
        const edge::ovl::Entry* ent = edge::ovl::find(ls, ptr);
        uint64_t head;
        std::memcpy(&head, ls + ptr, 8);
        char line[200];
        std::snprintf(line, sizeof line, "n%u %s {0x100, 0x%016llxull, 0x%016llxull}", d.native, ent ? edge::ovl::entry_name(size_t(edge::ovl::index_of(ent))) : "?", static_cast<unsigned long long>(head),
                      static_cast<unsigned long long>(edge::overlay_hash(ls + ptr, 0x100)));
        if (seen.insert(line).second) std::printf("%s\n", line);
    }
    return 0;
}

CommandReg reg_vcsig("vcsig", cmd_vcsig);

}
