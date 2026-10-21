#include "capture.h"
#include "fz_cloth.h"
#include "fz_shadows.h"
#include "op_cloth.h"
#include "op_shadows.h"
#include "unit.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

using cs::xo::Ctl;
using cs::xo::V;
using GenFn = void (*)(V*, uint8_t*, Ctl&);

struct GenEntry {
    uint32_t lsa;
    GenFn fn;
};

std::vector<GenEntry> all_gen(const std::string& job) {
    std::vector<GenEntry> v;
    if (job == "cloth")
        for (const auto& e : cloth::fz::kAllGen) v.push_back({e.lsa, e.fn});
    else
        for (const auto& e : shadows::fz::kAllGen) v.push_back({e.lsa, e.fn});
    return v;
}

struct OpE {
    uint32_t lsa;
    const char* name;
    GenFn fn;
};

std::vector<OpE> all_ops(const std::string& job) {
    std::vector<OpE> v;
    if (job == "cloth")
        for (const auto& e : cloth::opz::kOps) v.push_back({e.lsa, e.name, e.fn});
    else
        for (const auto& e : shadows::opz::kOps) v.push_back({e.lsa, e.name, e.fn});
    return v;
}

class NullHost : public spu::Host {
public:
    uint32_t read_channel(uint32_t) override { return 0; }
    void write_channel(uint32_t, uint32_t) override {}
    uint32_t channel_count(uint32_t) override { return 0; }
    bool stop_and_signal(uint32_t) override { return false; }
    void halt(uint32_t, uint32_t) override {}
};

uint32_t rnd_word(std::mt19937& rng) {
    static const uint32_t special[] = {0, 0x80000000u, 0x00000001u, 0x007fffffu, 0x00800000u, 0x7f800000u, 0xff800000u, 0x7f7fffffu, 0xff7fffffu, 0x3f800000u, 0xbf800000u, 0x7fffffffu, 0xffffffffu, 0x4effffffu, 0x4f000000u, 0xcf000000u, 0x0000ffffu, 0x00010000u, 0x10u, 0x1fu, 0x3fu, 0x80u};
    uint32_t r = rng();
    switch (r & 7) {
    case 0:
    case 1: return special[rng() % (sizeof(special) / 4)];
    case 2: return rng() & 0xff;
    case 3: return 0x20000 + ((rng() & 0x1fff) << 4) + (rng() & 15);
    case 4: return (rng() & 0x80000000u) | ((rng() % 255) << 23) | (rng() & 0x7fffff);
    default: return rng();
    }
}

}

int cs_fuzz(const std::string& job, const std::vector<uint8_t>& image, const std::vector<std::string>& args) {
    uint32_t only = args.size() > 1 && args[1] != "all" ? std::stoul(args[1], nullptr, 16) : 0;
    int iters = args.size() > 2 ? std::stoi(args[2]) : 200;
    uint32_t seed = args.size() > 3 ? std::stoul(args[3]) : 1;
    std::mt19937 rng(seed);
    int bad_total = 0;
    uint32_t data_lo = (0x4000 + uint32_t(image.size()) + 0x3ff) & ~0x3ffu;
    for (const auto& e : all_gen(job)) {
        if (only && e.lsa != only) continue;
        int ran = 0, declined = 0, bad = 0, iso_fail = 0, smc = 0, exits = 0;
        for (int it = 0; it < iters; it++) {
            edgeunit::Snapshot s;
            s.ls.assign(spu::LS_SIZE + 16, 0);
            for (uint32_t a = data_lo; a < spu::LS_SIZE; a += 4) {
                uint32_t r = rng();
                uint32_t v;
                switch (r & 7) {
                case 0: v = 0; break;
                case 1: v = 0x20000 + ((rng() & 0x1fff) << 4); break;
                case 2: v = rng() & 0xff; break;
                case 3: v = rng() & 0xffff; break;
                default: v = rng(); break;
                }
                std::memcpy(s.ls.data() + a, &v, 4);
            }
            std::memcpy(s.ls.data() + 0x4000, image.data(), image.size());
            s.regs = spu::Context{};
            for (int r = 0; r < 128; r++)
                for (int k = 0; k < 4; k++) {
                    uint32_t x = rng();
                    uint32_t v = (x & 3) == 0 ? 0x20000 + ((rng() & 0x1fff) << 4) : (x & 3) == 1 ? (rng() & 0x3f) : rng();
                    s.regs.gpr[r].u32[k] = v;
                }
            uint32_t sp = 0x3f000 + ((rng() & 0x7f) << 4);
            for (int k = 0; k < 4; k++) s.regs.gpr[1].u32[k] = k == 3 ? sp : rng();
            s.regs.gpr[0].u32[3] = 0x100;
            s.regs.pc = e.lsa;
            edgeunit::Snapshot nat = s;
            alignas(16) V R[128];
            std::memcpy(R, nat.regs.gpr, sizeof R);
            Ctl ctl;
            cs::xo::JournalStore js;
            js.attach(ctl);
            ctl.reset(400000);
            {
                spuf::CsrGuard g;
                e.fn(R, nat.ls.data(), ctl);
            }
            if (ctl.fail) {
                declined++;
                ctl.rollback(nat.ls.data());
                if (nat.ls != s.ls) {
                    bad++;
                    if (bad <= 3) {
                        edgeunit::LsDiff d = edgeunit::diff_ls(nat.ls.data(), s.ls.data(), 0, 0);
                        std::printf("  %05x it %d: rollback incomplete fail-pc %05x diff %zu [%05x..%05x]\n", e.lsa, it, ctl.fail_pc, d.bytes, d.first, d.last);
                    }
                }
                continue;
            }
            uint32_t exit_pc = uint32_t(_mm_extract_epi32(R[0], 3)) & spu::PC_MASK;
            uint32_t exit_sp = uint32_t(_mm_extract_epi32(R[1], 3));
            exits += ctl.out;
            edgeunit::Snapshot iso = s;
            edgeunit::IsoResult r = edgeunit::run_isolated(iso, exit_pc, exit_sp, 100000000);
            if (!r.ok) {
                iso_fail++;
                if (std::memcmp(iso.ls.data() + 0x4000, image.data(), image.size()) != 0) {
                    smc++;
                    continue;
                }
                bad++;
                if (bad <= 3) std::printf("  %05x it %d: native reached %05x sp %05x but SPU did not (%s)\n", e.lsa, it, exit_pc, exit_sp, r.error.c_str());
                continue;
            }
            if (std::memcmp(iso.ls.data() + 0x4000, image.data(), image.size()) != 0) {
                smc++;
                continue;
            }
            ran++;
            std::memcpy(nat.regs.gpr, R, sizeof R);
            edgeunit::LsDiff d = edgeunit::diff_ls(nat.ls.data(), iso.ls.data(), 0, 0);
            int rd = -1;
            for (int k = ctl.out ? 1 : 0; k < 128; k++)
                if (std::memcmp(&nat.regs.gpr[k], &iso.regs.gpr[k], 16) != 0) {
                    rd = k;
                    break;
                }
            if (d.bytes || rd >= 0) {
                bad++;
                if (bad <= 3) std::printf("  %05x it %d: ls diff %zu [%05x..%05x] reg r%d exit %d pc %05x\n", e.lsa, it, d.bytes, d.first, d.last, rd, int(ctl.out), exit_pc);
            }
        }
        std::printf("fuzz %05x: ran %d (exits %d) declined %d iso-fail %d self-modified %d bad %d\n", e.lsa, ran, exits, declined, iso_fail, smc, bad);
        bad_total += bad;
    }
    return bad_total ? 1 : 0;
}

int cs_opfuzz(const std::string& job, const std::vector<uint8_t>& image, const std::vector<std::string>& args) {
    int iters = args.size() > 1 ? std::stoi(args[1]) : 20000;
    uint32_t seed = args.size() > 2 ? std::stoul(args[2]) : 1;
    std::mt19937 rng(seed);
    int bad_total = 0;
    std::vector<uint8_t> ls0(spu::LS_SIZE + 16), ls1, ls2;
    for (uint32_t a = 0; a < spu::LS_SIZE; a++) ls0[a] = uint8_t(rng());
    std::memcpy(ls0.data() + 0x4000, image.data(), image.size());
    auto ops = all_ops(job);
    for (const auto& e : ops) {
        int bad = 0;
        for (int it = 0; it < iters; it++) {
            spu::Context c{};
            for (int r = 0; r < 128; r++)
                for (int k = 0; k < 4; k++) c.gpr[r].u32[k] = rnd_word(rng);
            if (rng() & 1)
                for (int r = 0; r < 128; r++) c.gpr[r].u32[2] = c.gpr[r].u32[1] = c.gpr[r].u32[0] = c.gpr[r].u32[3];
            c.pc = e.lsa;
            ls1 = ls0;
            ls2 = ls0;
            spu::Context ci = c;
            ci.ls = ls1.data();
            NullHost h;
            spu::step(ci, h);
            alignas(16) V R[128];
            std::memcpy(R, c.gpr, sizeof R);
            Ctl ctl;
            cs::xo::JournalStore js;
            js.attach(ctl);
            ctl.reset();
            {
                spuf::CsrGuard g;
                e.fn(R, ls2.data(), ctl);
            }
            int rd = -1;
            for (int k = 0; k < 128; k++)
                if (std::memcmp(&R[k], &ci.gpr[k], 16) != 0) {
                    rd = k;
                    break;
                }
            bool lsd = ls1 != ls2;
            if (rd >= 0 || lsd || ctl.fail) {
                bad++;
                if (bad <= 3) {
                    uint32_t w = (uint32_t(image[e.lsa - 0x4000]) << 24) | (uint32_t(image[e.lsa - 0x4000 + 1]) << 16) | (uint32_t(image[e.lsa - 0x4000 + 2]) << 8) | image[e.lsa - 0x4000 + 3];
                    std::printf("  op %s at %05x (%08x): reg r%d ls %d fail %d\n", e.name, e.lsa, w, rd, lsd, ctl.fail);
                    if (rd >= 0) {
                        uint32_t got[4];
                        std::memcpy(got, &R[rd], 16);
                        std::printf("    got %08x %08x %08x %08x want %08x %08x %08x %08x\n", got[3], got[2], got[1], got[0], ci.gpr[rd].u32[3], ci.gpr[rd].u32[2], ci.gpr[rd].u32[1], ci.gpr[rd].u32[0]);
                    }
                }
            }
        }
        if (bad) std::printf("opfuzz %s at %05x: bad %d of %d\n", e.name, e.lsa, bad, iters);
        bad_total += bad;
    }
    std::printf("opfuzz: %zu ops, %d bad\n", ops.size(), bad_total);
    return bad_total ? 1 : 0;
}

int cs_convfuzz(const std::vector<std::string>& args) {
    int iters = args.size() > 1 ? std::stoi(args[1]) : 2000;
    std::mt19937 rng(7);
    std::vector<uint8_t> ls(spu::LS_SIZE + 16, 0);
    int bad = 0;
    auto run = [&](uint32_t w, const spu::Context& c0, spu::Context& out) {
        out = c0;
        out.ls = ls.data();
        out.pc = 0x100;
        uint32_t be = __builtin_bswap32(w);
        std::memcpy(ls.data() + 0x100, &be, 4);
        NullHost h;
        spuf::CsrGuard g;
        spu::step(out, h);
    };
    auto check = [&](const char* name, int imm, uint32_t w, auto fn) {
        for (int it = 0; it < iters; it++) {
            spu::Context c{};
            for (int r = 0; r < 4; r++)
                for (int k = 0; k < 4; k++) c.gpr[r].u32[k] = rnd_word(rng);
            spu::Context ref;
            run(w, c, ref);
            alignas(16) V a, b, t;
            std::memcpy(&a, &c.gpr[1], 16);
            std::memcpy(&b, &c.gpr[2], 16);
            std::memcpy(&t, &c.gpr[3], 16);
            V got;
            {
                spuf::CsrGuard g;
                got = fn(a, b, t);
            }
            if (std::memcmp(&got, &ref.gpr[3], 16) != 0) {
                if (bad++ < 10) {
                    uint32_t gv[4];
                    std::memcpy(gv, &got, 16);
                    std::printf("  %s imm %d: in %08x %08x %08x %08x got %08x %08x %08x %08x want %08x %08x %08x %08x\n", name, imm, c.gpr[1].u32[3], c.gpr[1].u32[2], c.gpr[1].u32[1], c.gpr[1].u32[0], gv[3], gv[2], gv[1], gv[0], ref.gpr[3].u32[3], ref.gpr[3].u32[2], ref.gpr[3].u32[1], ref.gpr[3].u32[0]);
                }
            }
        }
    };
    using namespace cs::xo;
    for (int i8 = 0; i8 < 256; i8++) {
        check("cflts", i8, (0x1D8u << 22) | (uint32_t(i8) << 14) | (1u << 7) | 3u, [&](V a, V, V) { return cflts(a, i8); });
        check("cfltu", i8, (0x1D9u << 22) | (uint32_t(i8) << 14) | (1u << 7) | 3u, [&](V a, V, V) { return cfltu(a, i8); });
        check("csflt", i8, (0x1DAu << 22) | (uint32_t(i8) << 14) | (1u << 7) | 3u, [&](V a, V, V) { return csflt(a, i8); });
        check("cuflt", i8, (0x1DBu << 22) | (uint32_t(i8) << 14) | (1u << 7) | 3u, [&](V a, V, V) { return cuflt(a, i8); });
    }
    auto rr = [](uint32_t op) { return (op << 21) | (2u << 14) | (1u << 7) | 3u; };
    check("clz", 0, rr(0x2A5), [](V a, V, V) { return clz(a); });
    check("fsm", 0, rr(0x1B4), [](V a, V, V) { return fsm(a); });
    check("fsmh", 0, rr(0x1B5), [](V a, V, V) { return fsmh(a); });
    check("rotqbi", 0, rr(0x1D8), [](V a, V b, V) { return rotqbi(a, b); });
    check("rotqmbi", 0, rr(0x1D9), [](V a, V b, V) { return rotqmbi(a, b); });
    check("shlqbi", 0, rr(0x1DB), [](V a, V b, V) { return shlqbi(a, b); });
    check("cgx", 0, rr(0x342), [](V a, V b, V t) { return cgx(a, b, t); });
    check("fcgt", 0, rr(0x2C2), [](V a, V b, V) { return fcgt(a, b); });
    check("fceq", 0, rr(0x3C2), [](V a, V b, V) { return fceq(a, b); });
    check("fcmgt", 0, rr(0x2CA), [](V a, V b, V) { return fcmgt(a, b); });
    check("fcmeq", 0, rr(0x3CA), [](V a, V b, V) { return fcmeq(a, b); });
    check("bgx", 0, rr(0x343), [](V a, V b, V t) { return bgx(a, b, t); });
    for (int i7 = -64; i7 < 64; i7++) {
        uint32_t f = (uint32_t(i7) & 0x7f) << 14;
        check("rotqbii", i7, (0x1F8u << 21) | f | (1u << 7) | 3u, [&](V a, V, V) { return rotqbii(a, i7); });
        check("rotqmbii", i7, (0x1F9u << 21) | f | (1u << 7) | 3u, [&](V a, V, V) { return rotqmbii(a, i7); });
        check("shlqbii", i7, (0x1FBu << 21) | f | (1u << 7) | 3u, [&](V a, V, V) { return shlqbii(a, i7); });
    }
    std::printf("convfuzz: %d bad\n", bad);
    return bad ? 1 : 0;
}

bool cs_has_gen(const std::string& job, uint32_t pc) {
    for (const auto& e : all_gen(job))
        if (e.lsa == pc) return true;
    return false;
}

bool cs_gen_call(const std::string& job, uint32_t pc, uint8_t* ls, spu::Context& c) {
    for (const auto& e : all_gen(job)) {
        if (e.lsa != pc) continue;
        alignas(16) V R[128];
        std::memcpy(R, c.gpr, sizeof R);
        Ctl ctl;
        cs::xo::JournalStore js;
        js.attach(ctl);
        ctl.reset(1ull << 30);
        {
            spuf::CsrGuard g;
            e.fn(R, ls, ctl);
        }
        if (ctl.fail) {
            ctl.rollback(ls);
            return false;
        }
        std::memcpy(c.gpr, R, sizeof R);
        return true;
    }
    return false;
}
