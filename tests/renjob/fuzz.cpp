#include "capture.h"
#include "fz_gen.h"
#include "unit.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

int rj_fuzz(const std::vector<uint8_t>& image, const std::vector<std::string>& args) {
    uint32_t only = args.size() > 1 && args[1] != "all" ? std::stoul(args[1], nullptr, 16) : 0;
    int iters = args.size() > 2 ? std::stoi(args[2]) : 200;
    uint32_t seed = args.size() > 3 ? std::stoul(args[3]) : 1;
    std::mt19937 rng(seed);
    int bad_total = 0;
    renjob::xo::JournalStore js;
    for (const auto& e : renjob::fz::kAllGen) {
        if (only && e.lsa != only) continue;
        int ran = 0, declined = 0, bad = 0, iso_fail = 0, smc = 0, exits = 0;
        for (int it = 0; it < iters; it++) {
            edgeunit::Snapshot s;
            s.ls.assign(spu::LS_SIZE + 16, 0);
            for (uint32_t a = 0x18000; a < spu::LS_SIZE; a += 4) {
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
            alignas(16) renjob::xo::V R[128];
            std::memcpy(R, nat.regs.gpr, sizeof R);
            renjob::xo::Ctl ctl;
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
                    if (bad <= 3) std::printf("  %05x it %d: rollback incomplete\n", e.lsa, it);
                }
                continue;
            }
            ctl.clear_dirty();
            uint32_t exit_pc = 0x100, exit_sp = sp;
            uint32_t r0v, r1v;
            std::memcpy(&r0v, reinterpret_cast<const uint8_t*>(&R[0]) + 12, 4);
            std::memcpy(&r1v, reinterpret_cast<const uint8_t*>(&R[1]) + 12, 4);
            if (ctl.out) {
                exit_pc = r0v & 0x3fffc;
                exit_sp = r1v;
                exits++;
                if (std::getenv("RJ_FZDBG")) std::printf("    it %d exit pc %05x budget %llu\n", it, exit_pc, (unsigned long long)ctl.budget);
            }
            edgeunit::Snapshot iso = s;
            edgeunit::IsoResult r = edgeunit::run_isolated(iso, exit_pc, exit_sp, 400000);
            if (!r.ok) {
                iso_fail++;
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
                if (bad <= 3) std::printf("  %05x it %d: ls diff %zu [%05x..%05x] reg r%d exit %d pc %05x sp %05x iso-insns %llu\n", e.lsa, it, d.bytes, d.first, d.last, rd, int(ctl.out), exit_pc, exit_sp, (unsigned long long)r.insns);
            }
        }
        std::printf("fuzz %05x: ran %d (exits %d) declined %d iso-fail %d self-modified %d bad %d\n", e.lsa, ran, exits, declined, iso_fail, smc, bad);
        bad_total += bad;
    }
    return bad_total ? 1 : 0;
}

#include "op_gen.h"

namespace {

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

int rj_opfuzz(const std::vector<uint8_t>& image, const std::vector<std::string>& args) {
    int iters = args.size() > 1 ? std::stoi(args[1]) : 20000;
    uint32_t seed = args.size() > 2 ? std::stoul(args[2]) : 1;
    std::mt19937 rng(seed);
    int bad_total = 0;
    std::vector<uint8_t> ls0(spu::LS_SIZE + 16), ls1, ls2;
    for (uint32_t a = 0; a < spu::LS_SIZE; a++) ls0[a] = uint8_t(rng());
    std::memcpy(ls0.data() + 0x4000, image.data(), image.size());
    for (const auto& e : renjob::opz::kOps) {
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
            alignas(16) renjob::xo::V R[128];
            std::memcpy(R, c.gpr, sizeof R);
            renjob::xo::Ctl ctl;
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
                    uint32_t ra = (w >> 7) & 0x7f, rb = (w >> 14) & 0x7f, rc = w & 0x7f;
                    std::printf("  op %s at %05x (%08x): reg r%d ls %d fail %d\n", e.name, e.lsa, w, rd, lsd, ctl.fail);
                    if (rd >= 0) {
                        uint32_t got[4];
                        std::memcpy(got, &R[rd], 16);
                        std::printf("    got %08x %08x %08x %08x want %08x %08x %08x %08x\n", got[3], got[2], got[1], got[0], ci.gpr[rd].u32[3], ci.gpr[rd].u32[2], ci.gpr[rd].u32[1], ci.gpr[rd].u32[0]);
                        for (uint32_t r : {ra, rb, rc}) std::printf("    r%u %08x %08x %08x %08x\n", r, c.gpr[r].u32[3], c.gpr[r].u32[2], c.gpr[r].u32[1], c.gpr[r].u32[0]);
                    }
                }
            }
        }
        if (bad) std::printf("opfuzz %s at %05x: bad %d of %d\n", e.name, e.lsa, bad, iters);
        bad_total += bad;
    }
    std::printf("opfuzz: %zu ops, %d bad\n", sizeof(renjob::opz::kOps) / sizeof(renjob::opz::kOps[0]), bad_total);
    return bad_total ? 1 : 0;
}
