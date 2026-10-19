#include "natreg.h"
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

RunResult guarded_call(NativeFn fn, uint8_t* ls, spu::Context& c) {
    RunResult r;
    edge::Outputs outs;
    __try {
        r.handled = fn(ls, c, outs);
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
    switch (rnd() % 9) {
    case 0: return 0x3f000u + 16 * (rnd() % 0x100);
    case 1: return 0x3fff0u - 16 * (rnd() % 8);
    case 2: return 0x80000000u | (rnd() & 0x3fff0u);
    case 3: return rnd();
    case 4: return rnd() & 0xffff;
    case 5: return 0xfffffff0u - 16 * (rnd() % 64);
    case 6: return rnd() % 64;
    case 7: return 0x40000u + (rnd() & 0x3fff0u);
    default: return (rnd() & 0x3fff0u) | (rnd() & 15);
    }
}

void put32(uint8_t* p, uint32_t v) {
    uint32_t b = __builtin_bswap32(v);
    std::memcpy(p, &b, 4);
}

int cmd_robust(const std::vector<std::string>& args) {
    if (args.size() < 2) return 2;
    uint32_t fn = std::stoul(args[1], nullptr, 16);
    int count = args.size() > 2 ? std::stoi(args[2]) : 2000;
    g_state = args.size() > 3 ? std::stoull(args[3]) * 0x9E3779B97F4A7C15ull + 1 : 1;
    int maxbase = 40;
    std::vector<edgeunit::Hit> bases;
    edgeunit::for_each_call(harness::cap_list(args, 4), fn, [&](edgeunit::Hit& h) {
        bases.push_back(h);
        return int(bases.size()) < maxbase;
    }, harness::edge_loaded);
    if (bases.empty() || !natives().count(fn)) {
        std::printf("no hits or no native\n");
        return 2;
    }
    NativeFn nat = natives()[fn];
    GuardedLs g;
    int faults = 0, handled = 0, cmp = 0, bad = 0, spu_fail = 0;
    std::map<std::string, int> fault_kinds;
    for (int it = 0; it < count; it++) {
        const edgeunit::Hit& b = bases[size_t(rnd()) % bases.size()];
        edgeunit::Snapshot s = b.before;
        uint32_t ctx = s.regs.gpr[3].u32[3] & 0x3fff0;
        int muts = 1 + rnd() % 4;
        std::string desc;
        for (int m = 0; m < muts; m++) {
            int kind = rnd() % 3;
            if (kind == 0 && fn != 0xf538 && fn != 0xbbc8) {
                uint32_t off = 4 * (rnd() % 112);
                uint32_t v = evil_word();
                put32(s.ls.data() + ((ctx + off) & 0x3fffc), v);
                char d[48];
                std::snprintf(d, sizeof d, " ctx+%u=%08x", off, v);
                desc += d;
            } else if (kind == 1) {
                int r = 4 + rnd() % 5;
                uint32_t v = evil_word();
                s.regs.gpr[r].u32[3] = v;
                char d[48];
                std::snprintf(d, sizeof d, " r%d=%08x", r, v);
                desc += d;
            } else {
                uint32_t a = rnd() & 0x3fff0;
                if (a >= 0x3f000 || a < 0x4000 + 0xc430) continue;
                uint32_t v = evil_word();
                put32(s.ls.data() + a + 4 * (rnd() % 4), v);
            }
        }
        std::memcpy(g.ls, s.ls.data(), spu::LS_SIZE + 16 > s.ls.size() ? s.ls.size() : spu::LS_SIZE + 16);
        spu::Context nc = s.regs;
        nc.ls = g.ls;
        nc.line_epoch = nullptr;
        RunResult rr = guarded_call(nat, g.ls, nc);
        if (rr.faulted) {
            faults++;
            if (faults <= 10) std::printf("FAULT case %d base job %d:%s addr ls%+lld pc %llx\n", it, b.job, desc.c_str(), (long long)(rr.addr - uint64_t(g.ls)), (unsigned long long)rr.pc);
            continue;
        }
        if (!rr.handled) continue;
        if (std::getenv("ROBUST_NOSPU")) continue;
        handled++;
        edgeunit::Snapshot iso = s;
        edgeunit::IsoResult r = edgeunit::run_isolated(iso, b.ret, b.sp, 20000000);
        if (!r.ok) {
            spu_fail++;
            continue;
        }
        cmp++;
        edgeunit::LsDiff d = edgeunit::diff_ls(g.ls, iso.ls.data(), 0, 0);
        bool r3ok = std::memcmp(&nc.gpr[3], &iso.regs.gpr[3], 16) == 0;
        if (d.bytes || !r3ok) {
            bad++;
            if (bad <= 10) std::printf("MISMATCH case %d base job %d:%s ls_diff %zu [%05x..%05x] r3 %s\n", it, b.job, desc.c_str(), d.bytes, d.first, d.last, r3ok ? "ok" : "DIFF");
        }
    }
    std::printf("robust %05x: %d cases, faults %d, native handled %d, compared %d, mismatching %d, spu did not finish %d\n", fn, count, faults, handled, cmp, bad, spu_fail);
    return faults || bad ? 1 : 0;
}

CommandReg reg_robust("robust", cmd_robust);

}
