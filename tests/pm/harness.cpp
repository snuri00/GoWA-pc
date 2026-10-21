#include "capture.h"
#include "pm_adapters.h"
#include "ej_adapters.h"
#include "mh_adapters.h"
#include "an_adapters.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <xmmintrin.h>

#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {

struct Target {
    const char* name;
    const uint32_t* (*hooks)(size_t&);
    const uint32_t* (*signature)(uint32_t, uint32_t&);
    bool (*image_loaded)(const uint8_t*);
    bool (*run_hook)(spu::Context&, spu::Host&);
    uint64_t budget;
    uint32_t lo, hi;
    uint32_t data_lo, data_hi;
    uint32_t code_lo, code_hi;
    uint32_t code2_lo, code2_hi;
    uint32_t ea = 0, size = 0;
};

const Target kPm = {"pm", pm::hooks, pm::signature, pm::image_loaded, pm::run_hook, pm::kDefaultBudget, 0xa00, 0x39f0, 0xaf0, 0x16d0, 0xa00, 0xaf0, 0x16d0, 0x39d8};
const Target kEdge = {"edgejob", edgejob::hooks, edgejob::signature, edgejob::image_loaded, edgejob::run_hook, edgejob::kDefaultBudget, 0x4030, 0x10430, 0x11000, 0x29000, 0x4000, 0x10430, 0x4000, 0x4000};
const Target kMh = {"meshhier", meshhier::hooks, meshhier::signature, meshhier::image_loaded, meshhier::run_hook, meshhier::kDefaultBudget, 0x4030, 0xc130, 0xc200, 0x3f000, 0x4000, 0xc130, 0x4000, 0x4000, 0x78b380, 0x8130};
const Target kAn = {"anim", anim::hooks, anim::signature, anim::image_loaded, anim::run_hook, anim::kDefaultBudget, 0x4030, 0xea70, 0xeb00, 0x3f000, 0x4000, 0xea70, 0x4000, 0x4000, 0x84be40, 0xaa70};
const Target* T = &kPm;

uint32_t rbe32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

std::set<uint32_t> hook_set() {
    size_t n = 0;
    const uint32_t* h = T->hooks(n);
    return std::set<uint32_t>(h, h + n);
}

bool sig_ok(const uint8_t* ls, uint32_t pc) {
    uint32_t words = 0;
    const uint32_t* sig = T->signature(pc, words);
    for (uint32_t i = 0; i < words; i++)
        if (rbe32(ls + pc + 4 * i) != sig[i]) return false;
    return T->image_loaded(ls);
}

std::string state_diff(const edgecap::Machine& a, const edgecap::Machine& b) {
    std::string d;
    if (a.c.pc != b.c.pc) d += " pc " + std::to_string(a.c.pc) + "/" + std::to_string(b.c.pc);
    if (a.c.srr0 != b.c.srr0) d += " srr0";
    if (a.c.interrupts_enabled != b.c.interrupts_enabled) d += " ie";
    if (a.c.halted != b.c.halted) d += " halted";
    if (a.pos != b.pos) d += " events " + std::to_string(a.pos) + "/" + std::to_string(b.pos);
    if (a.n != b.n) d += " n";
    for (int r = 0; r < 128; r++)
        if (std::memcmp(&a.c.gpr[r], &b.c.gpr[r], 16) != 0) {
            d += " r" + std::to_string(r);
            break;
        }
    if (std::memcmp(a.ls.data(), b.ls.data(), spu::LS_SIZE) != 0)
        for (uint32_t l = 0; l < spu::LS_SIZE; l += 16)
            if (std::memcmp(a.ls.data() + l, b.ls.data() + l, 16) != 0) {
            char t[32];
            std::snprintf(t, sizeof t, " ls %05x", l);
            d += t;
            break;
        }
    if (a.error != b.error) d += " error[" + a.error + "|" + b.error + "]";
    return d;
}

int unit(const std::vector<std::string>& caps) {
    std::set<uint32_t> hooks = hook_set();
    uint64_t budget = std::getenv("PM_BUDGET") ? std::strtoull(std::getenv("PM_BUDGET"), nullptr, 0) : T->budget;
    int bad_total = 0;
    uint64_t all_pm = 0, all_cov = 0, all_calls = 0;
    std::map<uint32_t, uint64_t> uncovered;
    for (const std::string& path : caps) {
        edgecap::Capture cap;
        if (!cap.load(path)) {
            std::printf("%s: bad capture\n", path.c_str());
            bad_total++;
            continue;
        }
        edgecap::Machine ref(cap);
        uint64_t pm_insns = 0, covered = 0, calls = 0, declined = 0;
        int bad = 0;
        uint64_t skip_until = 0;
        while (!ref.done()) {
            uint32_t pc = ref.c.pc;
            bool in_pm = pc >= T->lo && pc < T->hi && T->image_loaded(ref.ls.data());
            if (!std::getenv("PM_UNIT_OFF") && in_pm && ref.n >= skip_until && hooks.count(pc) && sig_ok(ref.ls.data(), pc) && cap.insns - ref.n > budget + 200000) {
                edgecap::Machine nat(ref);
                nat.c.ls = nat.ls.data();
                uint64_t hb = budget;
                if (ref.pos < cap.events.size() && cap.events[ref.pos].kind == 5 && cap.events[ref.pos].value > ref.n && cap.events[ref.pos].value - ref.n < hb) hb = cap.events[ref.pos].value - ref.n;
                nat.c.executed = uint32_t(hb);
                bool handled = T->run_hook(nat.c, nat);
                if (!handled) {
                    declined++;
                } else {
                    calls++;
                    uint64_t k = nat.c.executed;
                    uint64_t target = nat.n + k;
                    std::string past;
                    if (nat.pos < cap.events.size() && cap.events[nat.pos].kind == 5 && cap.events[nat.pos].value < target) past = " native passed irq event";
                    nat.n = target;
                    nat.c.irq_check = false;
                    if (nat.pos < cap.events.size() && cap.events[nat.pos].kind == 5 && cap.events[nat.pos].value == nat.n) nat.next(5, 0, uint32_t(nat.n), true);
                    for (uint64_t i = 0; i < k && !ref.done(); i++) {
                        if (ref.c.pc >= T->lo && ref.c.pc < T->hi) {
                            pm_insns++;
                            covered++;
                        }
                        ref.step();
                    }
                    std::string d = state_diff(nat, ref) + past;
                    if (!d.empty()) {
                        bad++;
                        if (bad <= 5) std::printf("  %s: hook %05x at insn %llu (%llu insns):%s\n", path.c_str(), pc, (unsigned long long)(ref.n - k), (unsigned long long)k, d.c_str());
                    }
                    continue;
                }
            }
            if (in_pm) {
                pm_insns++;
                uncovered[pc]++;
            }
            ref.step();
        }
        std::string fin;
        bool ok = ref.verify_final(fin);
        std::printf("%-28s calls %7llu declined %6llu PM insns %9llu covered %9llu (%.2f%%) mismatches %d final %s\n", path.c_str(), (unsigned long long)calls, (unsigned long long)declined,
                    (unsigned long long)pm_insns, (unsigned long long)covered, pm_insns ? 100.0 * covered / pm_insns : 0.0, bad, ok ? "exact" : fin.c_str());
        bad_total += bad;
        all_pm += pm_insns;
        all_cov += covered;
        all_calls += calls;
    }
    std::vector<std::pair<uint64_t, uint32_t>> u;
    for (auto& [pc, c] : uncovered) u.push_back({c, pc});
    std::sort(u.rbegin(), u.rend());
    std::printf("total calls %llu PM insns %llu covered %.2f%%, uncovered top:", (unsigned long long)all_calls, (unsigned long long)all_pm, all_pm ? 100.0 * all_cov / all_pm : 0.0);
    for (size_t i = 0; i < u.size() && i < 12; i++) std::printf(" %05x:%llu", u[i].second, (unsigned long long)u[i].first);
    std::printf("\nmismatches %d\n", bad_total);
    return bad_total ? 1 : 0;
}

struct ScriptHost : spu::Host {
    spu::Context* c = nullptr;
    uint64_t seed = 0;
    uint32_t ops = 0;
    std::vector<uint64_t> log;
    uint64_t mix(uint64_t x) {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdull;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ull;
        return x ^ (x >> 33);
    }
    uint32_t value() {
        uint64_t h = mix(seed * 1000003u + ops);
        switch (h & 7) {
        case 0: return 0;
        case 1: return 1;
        case 2: return uint32_t(h >> 32) & 0xff;
        default: return uint32_t(h >> 32);
        }
    }
    void effects() {
        uint64_t h = mix(seed * 7919u + ops * 31u + 5);
        if ((h & 0xff) == 1) {
            c->srr0 = c->pc;
            c->pc = 0;
            c->interrupts_enabled = false;
        }
        if ((h & 0x3ff) == 2) c->halted = true;
    }
    uint32_t read_channel(uint32_t ch) override {
        uint32_t v = value();
        log.push_back((uint64_t(1) << 40) | (uint64_t(ch) << 32) | v);
        effects();
        ops++;
        return v;
    }
    void write_channel(uint32_t ch, uint32_t v) override {
        log.push_back((uint64_t(2) << 40) | (uint64_t(ch) << 32) | v);
        effects();
        ops++;
    }
    uint32_t channel_count(uint32_t ch) override {
        uint32_t v = value() & 3;
        log.push_back((uint64_t(3) << 40) | (uint64_t(ch) << 32) | v);
        effects();
        ops++;
        return v;
    }
    bool stop_and_signal(uint32_t code) override {
        log.push_back((uint64_t(4) << 40) | code);
        ops++;
        return true;
    }
    void halt(uint32_t pc, uint32_t w) override {
        log.push_back((uint64_t(5) << 40) | pc);
        c->halted = true;
    }
};

int fuzz(const std::vector<std::string>& args) {
    int iters = args.size() > 0 ? std::stoi(args[0]) : 200;
    uint32_t seed0 = args.size() > 1 ? std::stoul(args[1]) : 1;
    edgecap::Capture cap;
    std::string base = args.size() > 2 ? args[2] : std::string(GOWA_ROOT) + "/out/spucap_rj/cap_0.bin";
    if (!cap.load(base)) {
        std::printf("bad capture %s\n", base.c_str());
        return 1;
    }
    std::vector<uint8_t> image;
    if (T->size) {
        std::FILE* f = std::fopen((std::string(GOWA_ROOT) + "/bin/01.12/GOWA.patched.elf").c_str(), "rb");
        if (!f) return 1;
        image.resize(T->size);
        std::fseek(f, long(T->ea - 0x10000), SEEK_SET);
        if (std::fread(image.data(), 1, image.size(), f) != image.size()) return 1;
        std::fclose(f);
    }
    size_t nh = 0;
    const uint32_t* hooks = T->hooks(nh);
    std::mt19937 rng(seed0);
    int bad_total = 0;
    uint64_t runs = 0, exits_irq = 0, halts = 0, insns = 0, smc = 0;
    for (size_t hi = 0; hi < nh; hi++) {
        uint32_t hook = hooks[hi];
        int bad = 0;
        for (int it = 0; it < iters; it++) {
            std::vector<uint8_t> ls(cap.ls0, cap.ls0 + spu::LS_SIZE);
            if (T->size) std::memcpy(ls.data() + 0x4000, image.data(), image.size());
            ls.resize(spu::LS_SIZE + 16, 0);
            for (uint32_t a = T->data_lo; a < T->data_hi; a += 4) {
                if (rng() & 3) continue;
                uint32_t v = rng();
                uint32_t pick = rng() & 7;
                if (pick == 0) v = 0;
                else if (pick == 1) v = T->data_lo + (rng() % (T->data_hi - T->data_lo) & ~3u);
                else if (pick == 2) v &= 0xffff;
                std::memcpy(ls.data() + a, &v, 4);
            }
            for (uint32_t a = 0x80; a < 0x200; a += 4)
                if ((rng() & 7) == 0) {
                    uint32_t v = rng();
                    std::memcpy(ls.data() + a, &v, 4);
                }
            spu::Context c0{};
            for (int r = 0; r < 128; r++)
                for (int k = 0; k < 4; k++) {
                    uint32_t x = rng();
                    uint32_t v = (x & 3) == 0 ? ((rng() & 1) ? T->data_lo + (rng() % (T->data_hi - T->data_lo) & ~15u) : 0x11000 + (rng() % 0x2e000 & ~15u)) : (x & 3) == 1 ? (rng() & 0x3f) : rng();
                    c0.gpr[r].u32[k] = v;
                }
            c0.gpr[1].u32[3] = 0x3f000 + ((rng() & 0x7f) << 4);
            c0.pc = hook;
            c0.srr0 = 0x4000 + ((rng() & 0xfff) << 2);
            c0.interrupts_enabled = rng() & 1;
            std::vector<uint8_t> nls = ls, rls = ls;
            spu::Context nc = c0, rc = c0;
            nc.ls = nls.data();
            rc.ls = rls.data();
            ScriptHost nh_, rh_;
            nh_.c = &nc;
            rh_.c = &rc;
            nh_.seed = rh_.seed = rng();
            nc.executed = 20000;
            bool handled = T->run_hook(nc, nh_);
            if (!handled) {
                bad++;
                if (bad <= 3) std::printf("  hook %05x it %d declined\n", hook, it);
                continue;
            }
            runs++;
            uint64_t k = nc.executed;
            insns += k;
            if (nc.irq_check) exits_irq++;
            if (nc.halted) halts++;
            bool ok = true;
            for (uint64_t i = 0; i < k; i++) {
                if (rc.halted) {
                    ok = false;
                    break;
                }
                if (!spu::step(rc, rh_) && !rc.halted) break;
            }
            nc.irq_check = false;
            rc.irq_check = false;
            if (std::memcmp(rls.data() + T->code_lo, ls.data() + T->code_lo, T->code_hi - T->code_lo) != 0 || std::memcmp(rls.data() + T->code2_lo, ls.data() + T->code2_lo, T->code2_hi - T->code2_lo) != 0) {
                smc++;
                continue;
            }
            std::string d;
            if (!ok) d += " ref halted early";
            if (nc.pc != rc.pc) d += " pc";
            if (nc.srr0 != rc.srr0) d += " srr0";
            if (nc.interrupts_enabled != rc.interrupts_enabled) d += " ie";
            if (nc.halted != rc.halted) d += " halted";
            if (nh_.log != rh_.log) d += " channel log";
            for (int r = 0; r < 128; r++)
                if (std::memcmp(&nc.gpr[r], &rc.gpr[r], 16) != 0) {
                    d += " r" + std::to_string(r);
                    break;
                }
            if (nls != rls) d += " ls";
            if (!d.empty()) {
                bad++;
                if (bad <= 3) std::printf("  hook %05x it %d (%llu insns):%s\n", hook, it, (unsigned long long)k, d.c_str());
            }
        }
        bad_total += bad;
    }
    std::printf("fuzz: %zu hooks x %d, %llu runs, %llu insns, %llu irq exits, %llu halts, %llu self-modifying skipped, %d mismatches\n", nh, iters, (unsigned long long)runs, (unsigned long long)insns,
                (unsigned long long)exits_irq, (unsigned long long)halts, (unsigned long long)smc, bad_total);
    return bad_total ? 1 : 0;
}

}

int main(int argc, char** argv) {
    _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x6000u | 0x8040u);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::printf("usage: pm_harness unit caps... | fuzz [iters] [seed] [cap]\n");
        return 2;
    }
    std::string cmd = argv[1];
    std::vector<std::string> rest(argv + 2, argv + argc);
    if ((cmd == "--edge" || cmd == "--mh" || cmd == "--an") && argc > 2) {
        T = cmd == "--edge" ? &kEdge : cmd == "--mh" ? &kMh : &kAn;
        cmd = argv[2];
        rest.assign(argv + 3, argv + argc);
    }
    if (cmd == "unit") return unit(rest);
    if (cmd == "fuzz") return fuzz(rest);
    std::printf("unknown command %s\n", cmd.c_str());
    return 2;
}
