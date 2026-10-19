#include "capture.h"
#include "meshprep_adapters.h"
#include "unit.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

struct GuardedLs {
    uint8_t* base = nullptr;
    uint8_t* ls = nullptr;
    GuardedLs() {
        size_t total = 0x1000 + 0x41000 + 0x1000;
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, total, MEM_RESERVE, PAGE_NOACCESS));
        VirtualAlloc(base + 0x1000, 0x41000, MEM_COMMIT, PAGE_READWRITE);
        ls = base + 0x1000 + 0x1000 - 16;
    }
    ~GuardedLs() { VirtualFree(base, 0, MEM_RELEASE); }
};

struct RunResult {
    bool faulted = false;
    bool handled = false;
    uint64_t addr = 0;
    uint64_t pc = 0;
};

uint64_t g_fault_addr = 0, g_fault_pc = 0;

int filter(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    g_fault_addr = ep->ExceptionRecord->ExceptionInformation[1];
    g_fault_pc = uint64_t(ep->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}

RunResult guarded_call(meshprep::AdapterFn fn, uint8_t* ls, spu::Context& c) {
    RunResult r;
    __try {
        r.handled = fn(ls, c);
    } __except (filter(GetExceptionInformation())) {
        r.faulted = true;
        r.addr = g_fault_addr;
        r.pc = g_fault_pc;
    }
    return r;
}

uint64_t g_state = 1;
uint32_t rnd() {
    g_state ^= g_state << 13;
    g_state ^= g_state >> 7;
    g_state ^= g_state << 17;
    return uint32_t(g_state >> 16);
}

uint32_t evil_word() {
    switch (rnd() % 10) {
    case 0: return 0x3f000u + 16 * (rnd() % 0x100);
    case 1: return 0x3fff0u - 16 * (rnd() % 8);
    case 2: return 0x80000000u | (rnd() & 0x3fff0u);
    case 3: return rnd();
    case 4: return rnd() & 0xffff;
    case 5: return 0xfffffff0u - 16 * (rnd() % 64);
    case 6: return rnd() % 64;
    case 7: return 0x40000u + (rnd() & 0x3fff0u);
    case 8: return 0x7fffffffu - (rnd() & 0xff);
    default: return (rnd() & 0x3fff0u) | (rnd() & 15);
    }
}

void put32(uint8_t* p, uint32_t v) {
    uint32_t b = __builtin_bswap32(v);
    std::memcpy(p, &b, 4);
}

uint32_t get32(const uint8_t* p) {
    uint32_t b;
    std::memcpy(&b, p, 4);
    return __builtin_bswap32(b);
}

}

int mp_robust(int job, const std::vector<uint8_t>& image, const std::vector<std::string>& args, const std::vector<std::string>& caps) {
    std::map<uint32_t, meshprep::AdapterFn> nats;
    size_t n = 0;
    const meshprep::Adapter* a = meshprep::adapters(n);
    for (size_t i = 0; i < n; i++)
        if (a[i].job == job) nats[a[i].lsa] = a[i].handler;
    uint32_t only = args.size() > 1 && args[1] != "all" ? std::stoul(args[1], nullptr, 16) : 0;
    int count = args.size() > 2 ? std::stoi(args[2]) : 2000;
    uint64_t seed = args.size() > 3 ? std::stoull(args[3]) : 1;
    size_t maxbase = std::getenv("MP_ROBUST_BASES") ? std::strtoul(std::getenv("MP_ROBUST_BASES"), nullptr, 10) : 40;
    std::map<uint32_t, std::vector<edgeunit::Snapshot>> bases;
    for (const std::string& path : caps) {
        edgecap::Capture cap;
        if (!cap.load(path)) continue;
        edgecap::Machine m(cap);
        while (!m.done()) {
            uint32_t pc = m.c.pc;
            if (nats.count(pc) && (!only || pc == only) && bases[pc].size() < maxbase && meshprep::image_loaded(job, m.c.ls)) {
                edgeunit::Snapshot s;
                s.ls.assign(m.ls.begin(), m.ls.end());
                s.regs = m.c;
                bases[pc].push_back(std::move(s));
            }
            m.step();
        }
    }
    uint32_t img_end = (0x4000 + uint32_t(image.size()) + 15) & ~15u;
    uint32_t code_end = meshprep::job_info(job).code_end;
    uint32_t span = (spu::LS_SIZE - img_end) / 4;
    GuardedLs g;
    meshprep::set_loop_budget(std::getenv("MP_BUDGET") ? std::strtoull(std::getenv("MP_BUDGET"), nullptr, 10) : 200000);
    int total_faults = 0, total_bad = 0;
    for (auto& [fn, bs] : bases) {
        if (bs.empty()) continue;
        g_state = seed * 0x9E3779B97F4A7C15ull + fn;
        meshprep::AdapterFn nat = nats[fn];
        int faults = 0, handled = 0, declined = 0, cmp = 0, bad = 0, spu_fail = 0, smc = 0, budget = 0, spu_long = 0;
        for (int it = 0; it < count; it++) {
            if (std::getenv("MP_ROBUST_TRACE")) {
                std::fprintf(stderr, "%05x case %d\n", fn, it);
                std::fflush(stderr);
            }
            const edgeunit::Snapshot& b = bs[size_t(rnd()) % bs.size()];
            edgeunit::Snapshot s = b;
            s.ls.resize(spu::LS_SIZE + 16);
            int muts = 1 + rnd() % 6;
            std::string desc;
            for (int k = 0; k < muts; k++) {
                int kind = rnd() % 4;
                char d[64];
                if (kind == 0) {
                    int r = 2 + rnd() % 80;
                    uint32_t v = evil_word();
                    s.regs.gpr[r].u32[3] = v;
                    std::snprintf(d, sizeof d, " r%d=%08x", r, v);
                    desc += d;
                } else if (kind == 1) {
                    uint32_t base = s.regs.gpr[3 + rnd() % 6].u32[3] & 0x3fff0;
                    uint32_t ad = (base + 4 * (rnd() % 128)) & 0x3fffc;
                    if (ad < img_end) continue;
                    uint32_t v = evil_word();
                    put32(s.ls.data() + ad, v);
                    std::snprintf(d, sizeof d, " [%05x]=%08x", ad, v);
                    desc += d;
                } else if (kind == 2) {
                    uint32_t ad = 0;
                    for (int t = 0; t < 64 && !ad; t++) {
                        uint32_t q = img_end + (rnd() % span) * 4;
                        uint32_t w = get32(s.ls.data() + q);
                        if (w >= 0x4000 && w < 0x40000) ad = q;
                    }
                    if (!ad) continue;
                    uint32_t v = evil_word();
                    put32(s.ls.data() + ad, v);
                    std::snprintf(d, sizeof d, " ptr[%05x]=%08x", ad, v);
                    desc += d;
                } else {
                    uint32_t ad = img_end + (rnd() % span) * 4;
                    uint32_t v = evil_word();
                    put32(s.ls.data() + ad, v);
                    std::snprintf(d, sizeof d, " w[%05x]=%08x", ad, v);
                    desc += d;
                }
            }
            {
                edgeunit::Snapshot pre = s;
                edgeunit::IsoResult pr = edgeunit::run_isolated(pre, s.regs.gpr[0].u32[3] & spu::PC_MASK, s.regs.gpr[1].u32[3], 20000000);
                if (std::getenv("MP_ROBUST_TRACE")) {
                    std::fprintf(stderr, "  %s spu %s %llu insns\n", desc.c_str(), pr.ok ? "ok" : pr.error.c_str(), (unsigned long long)pr.insns);
                    std::fflush(stderr);
                }
                if (!pr.ok && pr.error == "timeout") {
                    spu_long++;
                    continue;
                }
                if (std::memcmp(pre.ls.data() + 0x4000, s.ls.data() + 0x4000, code_end - 0x4000) != 0) {
                    smc++;
                    continue;
                }
            }
            std::memcpy(g.ls, s.ls.data(), spu::LS_SIZE + 16);
            spu::Context nc = s.regs;
            nc.ls = g.ls;
            nc.line_epoch = nullptr;
            uint32_t be0 = meshprep::budget_exits();
            RunResult rr = guarded_call(nat, g.ls, nc);
            if (rr.faulted) {
                faults++;
                if (faults <= 10) std::printf("FAULT %05x case %d:%s addr ls%+lld pc %llx\n", fn, it, desc.c_str(), (long long)(rr.addr - uint64_t(g.ls)), (unsigned long long)rr.pc);
                continue;
            }
            if (!rr.handled) {
                declined++;
                bool same = std::memcmp(g.ls, s.ls.data(), spu::LS_SIZE) == 0 && std::memcmp(nc.gpr, s.regs.gpr, sizeof nc.gpr) == 0;
                if (!same) {
                    bad++;
                    if (bad <= 10) std::printf("DECLINE CHANGED STATE %05x case %d:%s\n", fn, it, desc.c_str());
                }
                continue;
            }
            handled++;
            if (meshprep::budget_exits() != be0) {
                budget++;
                continue;
            }
            if (std::getenv("MP_ROBUST_NOSPU")) continue;
            uint32_t exit_pc = nc.gpr[0].u32[3] & spu::PC_MASK, exit_sp = nc.gpr[1].u32[3];
            bool region = exit_pc != (s.regs.gpr[0].u32[3] & spu::PC_MASK);
            edgeunit::Snapshot iso = s;
            edgeunit::IsoResult r = edgeunit::run_isolated(iso, exit_pc, exit_sp, 20000000);
            if (!r.ok) {
                spu_fail++;
                continue;
            }
            if (std::memcmp(iso.ls.data() + 0x4000, s.ls.data() + 0x4000, code_end - 0x4000) != 0) {
                smc++;
                continue;
            }
            cmp++;
            edgeunit::LsDiff dd = edgeunit::diff_ls(g.ls, iso.ls.data(), 0, 0);
            int rd = -1;
            for (int k = region ? 1 : 0; k < 128; k++)
                if (std::memcmp(&nc.gpr[k], &iso.regs.gpr[k], 16) != 0) {
                    rd = k;
                    break;
                }
            if (dd.bytes || rd >= 0) {
                bad++;
                if (bad <= 10) std::printf("MISMATCH %05x case %d:%s ls_diff %zu [%05x..%05x] reg r%d\n", fn, it, desc.c_str(), dd.bytes, dd.first, dd.last, rd);
            }
        }
        std::printf("robust %05x: bases %zu cases %d faults %d handled %d declined %d compared %d mismatching %d spu-not-reaching-exit %d self-modified %d budget-exits %d spu-over-20M-skipped %d\n", fn, bs.size(), count, faults, handled, declined, cmp, bad, spu_fail, smc, budget, spu_long);
        std::fflush(stdout);
        total_faults += faults;
        total_bad += bad;
    }
    std::printf("robust total: faults %d mismatching %d\n", total_faults, total_bad);
    return total_faults || total_bad ? 1 : 0;
}
