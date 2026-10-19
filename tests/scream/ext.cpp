#include "capture.h"
#include "images.h"
#include "scream_adapters.h"
#include "spu_decode.h"
#include "unit.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <intrin.h>

std::vector<std::string> sc_cap_list(const std::vector<std::string>& args, size_t from);
int sc_fuzz(const std::vector<std::string>& args);
int sc_opfuzz(const std::vector<std::string>& args);

namespace {

using NativeFn = bool (*)(uint8_t* ls, spu::Context& c);

struct Native {
    NativeFn fn;
    int image;
    const char* name;
};

std::map<uint32_t, std::vector<Native>>& natives() {
    static std::map<uint32_t, std::vector<Native>> m = [] {
        std::map<uint32_t, std::vector<Native>> t;
        size_t n = 0;
        const scream::Adapter* a = scream::adapters(n);
        for (size_t i = 0; i < n; i++) t[a[i].lsa].push_back({a[i].handler, a[i].image, a[i].name});
        return t;
    }();
    return m;
}

const Native* native_at(const uint8_t* ls, uint32_t pc) {
    auto it = natives().find(pc);
    if (it == natives().end()) return nullptr;
    for (const Native& n : it->second)
        if (scream::image_loaded(ls, n.image)) return &n;
    return nullptr;
}

std::vector<uint32_t> parse_list(const std::string& list) {
    std::vector<uint32_t> v;
    size_t p = 0;
    while (p < list.size()) {
        size_t e = list.find(',', p);
        if (e == std::string::npos) e = list.size();
        v.push_back(std::stoul(list.substr(p, e - p), nullptr, 16));
        p = e + 1;
    }
    return v;
}

int regs_diff(const spu::Context& a, const spu::Context& b, bool skip_r0) {
    for (int i = skip_r0 ? 1 : 0; i < 128; i++)
        if (std::memcmp(&a.gpr[i], &b.gpr[i], 16) != 0) return i;
    return -1;
}

int unit_one(uint32_t fn, int maxhits, const std::vector<std::string>& caps) {
    bool verbose = std::getenv("SC_VERBOSE") != nullptr;
    if (!natives().count(fn)) {
        std::printf("no native for %05x\n", fn);
        return 1;
    }
    int hits = 0, nat_bad = 0, declined = 0, iso_fail = 0, region_exits = 0;
    uint64_t spu_insns = 0;
    double nat_sec = 0;
    std::map<uint32_t, int> decline_pcs, exit_pcs;
    std::string name;
    for (const std::string& path : caps) {
        edgecap::Capture cap;
        if (!cap.load(path)) continue;
        edgecap::Machine m(cap);
        while (!m.done() && hits < maxhits) {
            const Native* nat = m.c.pc == fn ? native_at(m.c.ls, fn) : nullptr;
            if (nat) {
                name = nat->name;
                hits++;
                edgeunit::Snapshot ns;
                ns.ls.assign(m.ls.begin(), m.ls.end());
                ns.regs = m.c;
                ns.regs.ls = ns.ls.data();
                auto t0 = std::chrono::steady_clock::now();
                bool ok = nat->fn(ns.ls.data(), ns.regs);
                nat_sec += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                if (!ok) {
                    declined++;
                    decline_pcs[scream::last_decline_pc()]++;
                    bool ls_same = std::memcmp(ns.ls.data(), m.ls.data(), spu::LS_SIZE) == 0;
                    int rd = regs_diff(ns.regs, m.c, false);
                    if (!ls_same || rd >= 0) {
                        nat_bad++;
                        std::printf("  decline left changes %s insn %llu: ls %s reg r%d\n", path.c_str(), (unsigned long long)m.n, ls_same ? "same" : "DIFF", rd);
                    }
                } else {
                    uint32_t exit_pc = ns.regs.gpr[0].u32[3] & spu::PC_MASK, exit_sp = ns.regs.gpr[1].u32[3];
                    bool region = exit_pc != (m.c.gpr[0].u32[3] & spu::PC_MASK);
                    region_exits += region;
                    exit_pcs[exit_pc]++;
                    edgeunit::Snapshot iso;
                    iso.ls.assign(m.ls.begin(), m.ls.begin() + spu::LS_SIZE);
                    iso.regs = m.c;
                    edgeunit::IsoResult r = edgeunit::run_isolated(iso, exit_pc, exit_sp);
                    if (!r.ok) {
                        iso_fail++;
                        nat_bad++;
                        if (iso_fail <= 5) std::printf("  native ran but SPU does not reach exit %05x purely (%s) %s insn %llu\n", exit_pc, r.error.c_str(), path.c_str(), (unsigned long long)m.n);
                    } else {
                        spu_insns += r.insns;
                        edgeunit::LsDiff d = edgeunit::diff_ls(ns.ls.data(), iso.ls.data(), 0, 0);
                        int rd = regs_diff(ns.regs, iso.regs, region);
                        if (d.bytes || rd >= 0) {
                            nat_bad++;
                            if (nat_bad <= 8 || verbose) {
                                std::printf("  native mismatch %s insn %llu exit %05x: ls_diff %zu [%05x..%05x] reg r%d\n", path.c_str(), (unsigned long long)m.n, exit_pc, d.bytes, d.first, d.last, rd);
                                if (rd >= 0) std::printf("    native %08x %08x %08x %08x spu %08x %08x %08x %08x\n", ns.regs.gpr[rd].u32[3], ns.regs.gpr[rd].u32[2], ns.regs.gpr[rd].u32[1], ns.regs.gpr[rd].u32[0], iso.regs.gpr[rd].u32[3], iso.regs.gpr[rd].u32[2], iso.regs.gpr[rd].u32[1], iso.regs.gpr[rd].u32[0]);
                            }
                        }
                    }
                }
            }
            m.step();
        }
    }
    std::printf("fn %05x %-22s hits %d ran %d (region exits %d) spu-insns %llu (%.0f/run) declined %d native-mismatch %d native-time %.3f ms (%.2f us/hit)\n", fn, name.c_str(), hits, hits - declined, region_exits, (unsigned long long)spu_insns,
                hits - declined ? double(spu_insns) / (hits - declined) : 0.0, declined, nat_bad, nat_sec * 1e3, hits ? nat_sec * 1e6 / hits : 0.0);
    for (auto& [pc, k] : decline_pcs) std::printf("  declined at %05x: %d\n", pc, k);
    if (verbose)
        for (auto& [pc, k] : exit_pcs) std::printf("  exit %05x: %d\n", pc, k);
    return nat_bad ? 1 : 0;
}

int cmd_unit(const std::vector<std::string>& args) {
    if (args.size() < 2) return 2;
    int maxhits = args.size() > 2 ? std::stoi(args[2]) : 1000000;
    std::vector<uint32_t> fns;
    if (args[1] == "all")
        for (auto& [k, v] : natives()) fns.push_back(k);
    else
        fns = parse_list(args[1]);
    int bad = 0;
    for (uint32_t fn : fns) bad += unit_one(fn, maxhits, sc_cap_list(args, 3));
    return bad ? 1 : 0;
}

struct PutRec {
    uint32_t cmd = 0, pc = 0, lsa = 0, ea = 0, size = 0;
    std::vector<uint8_t> data;
};

struct HookStat {
    uint64_t calls = 0, insns = 0, cycles = 0;
};

std::vector<PutRec> run_with_hooks(const std::string& path, const std::set<uint32_t>& hooks, uint64_t* hooked, uint64_t* skipped, uint64_t* declined, std::string* err, std::map<uint32_t, HookStat>* hs, std::map<std::pair<int, uint32_t>, uint64_t>* hist, uint64_t* scream_insns) {
    std::vector<PutRec> puts;
    edgecap::Capture cap;
    if (!cap.load(path)) return puts;
    scimg::TrackedMachine m(cap);
    m.on_dma = [&](const edgecap::Dma& d, bool after) {
        if (after) return;
        bool put = (d.cmd & 0xf0) == 0x20 || (d.cmd & 0xf0) == 0x30 || d.cmd == 0xb4 || d.cmd == 0xb0 || d.cmd == 0xb8;
        if (!put) return;
        PutRec p{d.cmd, d.pc, d.lsa, d.ea, d.size, {}};
        if (d.cmd >= 0xb0) p.data.assign(m.ls.begin() + (d.lsa & 0x3ff80), m.ls.begin() + (d.lsa & 0x3ff80) + 128);
        else if (d.cmd & 4) {
            uint32_t list = d.ea & 0x3fff8;
            uint32_t cur = d.lsa & 0x3fff0;
            for (uint32_t k = 0; k + 8 <= d.size; k += 8) {
                uint32_t sz = scimg::be32(m.ls.data() + ((list + k) & 0x3fff8)) & 0x7fff;
                uint32_t eea = scimg::be32(m.ls.data() + ((list + k + 4) & 0x3fffc));
                uint32_t lsa = cur | (eea & 0xf);
                for (uint32_t i = 0; i < sz; i++) p.data.push_back(m.ls[(lsa + i) & 0x3ffff]);
                cur = (cur + ((sz + 15) & ~15u)) & 0x3fff0;
            }
        } else if (d.size <= 0x4000)
            for (uint32_t i = 0; i < d.size; i++) p.data.push_back(m.ls[(d.lsa + i) & 0x3ffff]);
        puts.push_back(std::move(p));
    };
    while (!m.done()) {
        uint32_t pc = m.c.pc;
        const Native* nat = !hooks.empty() && hooks.count(pc) ? native_at(m.c.ls, pc) : nullptr;
        if (nat) {
            edgeunit::Snapshot iso;
            iso.ls.assign(m.ls.begin(), m.ls.begin() + spu::LS_SIZE);
            iso.regs = m.c;
            std::vector<uint8_t> orig_ls = iso.ls;
            spu::Context nc = m.c;
            uint64_t t0 = __rdtsc();
            bool ok = nat->fn(m.ls.data(), nc);
            uint64_t dt = __rdtsc() - t0;
            if (ok) {
                uint32_t exit_pc = nc.gpr[0].u32[3] & spu::PC_MASK, exit_sp = nc.gpr[1].u32[3];
                bool region = exit_pc != (m.c.gpr[0].u32[3] & spu::PC_MASK);
                edgeunit::IsoResult r = edgeunit::run_isolated(iso, exit_pc, exit_sp);
                if (r.ok && m.n + r.insns < cap.insns) {
                    if (err && err->empty()) {
                        int rd = regs_diff(nc, iso.regs, region);
                        char b[96];
                        if (rd >= 0) {
                            std::snprintf(b, sizeof b, "regs differ at hook %05x r%d", pc, rd);
                            *err = b;
                        } else if (std::memcmp(iso.ls.data(), m.ls.data(), spu::LS_SIZE) != 0) {
                            std::snprintf(b, sizeof b, "ls differs at hook %05x", pc);
                            *err = b;
                        }
                    }
                    uint8_t* keep = m.c.ls;
                    m.c = nc;
                    m.c.ls = keep;
                    m.c.pc = exit_pc;
                    m.tskip(r.insns);
                    if (hooked) (*hooked)++;
                    if (skipped) *skipped += r.insns;
                    if (scream_insns) *scream_insns += r.insns;
                    if (hs) {
                        HookStat& s = (*hs)[pc];
                        s.calls++;
                        s.insns += r.insns;
                        s.cycles += dt;
                    }
                    continue;
                }
                if (!r.ok && err && err->empty()) *err = "native ran where SPU is impure at hook " + std::to_string(pc);
                std::memcpy(m.ls.data(), orig_ls.data(), spu::LS_SIZE);
            } else if (declined)
                (*declined)++;
        }
        if (hist || scream_insns) {
            uint32_t base = 0;
            int img = m.tr.find(m.c.ls, pc, &base);
            if (img >= 0) {
                if (hist) (*hist)[{img, pc - base}]++;
                if (scream_insns) (*scream_insns)++;
            }
        }
        m.tstep();
    }
    if (err && err->empty() && !m.error.empty()) *err = m.error;
    if (err && !hooks.empty()) {
        std::string diff;
        if (!m.verify_final(diff)) *err += " final-state:" + diff;
    }
    return puts;
}

int cmd_e2e(const std::vector<std::string>& args) {
    if (args.size() < 2) return 2;
    std::set<uint32_t> hooks;
    if (args[1] == "all") {
        for (auto& [k, v] : natives()) hooks.insert(k);
    } else
        for (uint32_t h : parse_list(args[1])) hooks.insert(h);
    if (const char* skip = std::getenv("SC_SKIP"))
        for (uint32_t h : parse_list(skip)) hooks.erase(h);
    int bad = 0;
    std::map<std::pair<int, uint32_t>, uint64_t> hist;
    std::map<uint32_t, HookStat> hs;
    uint64_t tot_scream = 0, tot_skipped = 0;
    bool want_hist = std::getenv("SC_HIST") != nullptr;
    for (const std::string& path : sc_cap_list(args, 2)) {
        std::vector<PutRec> ref = run_with_hooks(path, {}, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        uint64_t hooked = 0, skipped = 0, declined = 0, scream_insns = 0;
        std::string err;
        std::vector<PutRec> got = run_with_hooks(path, hooks, &hooked, &skipped, &declined, &err, &hs, want_hist ? &hist : nullptr, &scream_insns);
        size_t mism = 0;
        std::map<uint32_t, size_t> mism_pc;
        for (size_t i = 0; i < std::min(ref.size(), got.size()); i++) {
            const PutRec& a = ref[i];
            const PutRec& b = got[i];
            if (a.cmd != b.cmd || a.ea != b.ea || a.size != b.size || a.data != b.data) {
                mism++;
                mism_pc[a.pc]++;
                if (mism <= 5) {
                    size_t off = 0;
                    while (off < a.data.size() && off < b.data.size() && a.data[off] == b.data[off]) off++;
                    std::printf("  put %zu mismatch pc %05x cmd %02x ea %08x size %x vs ea %08x size %x first-diff-byte %zx\n", i, a.pc, a.cmd, a.ea, a.size, b.ea, b.size, off);
                }
            }
        }
        tot_scream += scream_insns;
        tot_skipped += skipped;
        std::printf("%s: puts ref %zu got %zu mismatching %zu hooked %llu declined %llu replaced-insns %llu of scream %llu (%.1f%%) %s\n", path.c_str(), ref.size(), got.size(), mism, (unsigned long long)hooked, (unsigned long long)declined, (unsigned long long)skipped,
                    (unsigned long long)scream_insns, 100.0 * skipped / std::max<uint64_t>(1, scream_insns), err.c_str());
        for (auto& [pc, n] : mism_pc) std::printf("    mismatches at pc %05x: %zu\n", pc, n);
        if (mism || ref.size() != got.size() || !err.empty()) bad++;
    }
    std::printf("total: replaced %llu of %llu scream insns (%.1f%%)\n", (unsigned long long)tot_skipped, (unsigned long long)tot_scream, 100.0 * tot_skipped / std::max<uint64_t>(1, tot_scream));
    if (std::getenv("SC_HOOKS")) {
        std::vector<std::pair<uint32_t, HookStat>> v(hs.begin(), hs.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.insns > b.second.insns; });
        for (auto& [pc, s] : v)
            std::printf("  hook %05x calls %7llu spu-insns %10llu (%.0f/call) cycles/call %.0f cycles/insn %.2f\n", pc, (unsigned long long)s.calls, (unsigned long long)s.insns, double(s.insns) / std::max<uint64_t>(1, s.calls), double(s.cycles) / std::max<uint64_t>(1, s.calls),
                        double(s.cycles) / std::max<uint64_t>(1, s.insns));
    }
    if (const char* hf = std::getenv("SC_HIST"))
        if (std::FILE* f = std::fopen(hf, "w")) {
            for (auto& [k, n] : hist) std::fprintf(f, "%s %05x %llu\n", scimg::images()[k.first].name, k.second, (unsigned long long)n);
            std::fclose(f);
        }
    return bad ? 1 : 0;
}

int cmd_bench(const std::vector<std::string>& args) {
    if (args.size() < 2) return 2;
    uint32_t fn = std::stoul(args[1], nullptr, 16);
    int maxhits = args.size() > 2 ? std::stoi(args[2]) : 200;
    double total = 0;
    uint64_t insns = 0;
    int n = 0;
    for (const std::string& path : sc_cap_list(args, 3)) {
        edgecap::Capture cap;
        if (!cap.load(path)) continue;
        edgecap::Machine m(cap);
        while (!m.done() && n < maxhits) {
            const Native* nat = m.c.pc == fn ? native_at(m.c.ls, fn) : nullptr;
            if (nat) {
                std::vector<uint8_t> ls(m.ls.begin(), m.ls.end());
                spu::Context c0 = m.c;
                c0.ls = ls.data();
                if (!nat->fn(ls.data(), c0)) {
                    m.step();
                    continue;
                }
                edgeunit::Snapshot iso;
                iso.ls.assign(m.ls.begin(), m.ls.begin() + spu::LS_SIZE);
                iso.regs = m.c;
                edgeunit::IsoResult r = edgeunit::run_isolated(iso, c0.gpr[0].u32[3] & spu::PC_MASK, c0.gpr[1].u32[3]);
                double best = 1e18;
                for (int k = 0; k < 10; k++) {
                    std::memcpy(ls.data(), m.ls.data(), spu::LS_SIZE);
                    spu::Context c = m.c;
                    c.ls = ls.data();
                    uint64_t t0 = __rdtsc();
                    nat->fn(ls.data(), c);
                    best = std::min(best, double(__rdtsc() - t0));
                }
                total += best;
                insns += r.ok ? r.insns : 0;
                n++;
            }
            m.step();
        }
    }
    std::printf("fn %05x: %d hits, %.0f cycles/call best (hot), %.0f spu insns/call, %.2f cycles per SPU insn\n", fn, n, n ? total / n : 0.0, n ? double(insns) / n : 0.0, insns ? total / insns : 0.0);
    return 0;
}

}

int sc_main_ext(const std::string& cmd, const std::vector<std::string>& args, int& rc) {
    if (cmd == "unit") rc = cmd_unit(args);
    else if (cmd == "e2e") rc = cmd_e2e(args);
    else if (cmd == "bench") rc = cmd_bench(args);
    else if (cmd == "fuzz") rc = sc_fuzz(args);
    else if (cmd == "opfuzz") rc = sc_opfuzz(args);
    else return 0;
    return 1;
}
