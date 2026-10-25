#include "jobs_inplace.h"
#include "jobs_whole.h"

#include "runtime/log.h"
#include "runtime/memory.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace wj {

namespace {

struct Op {
    uint8_t kind;
    uint32_t ch;
    uint32_t value;
    uint32_t aux;
};

struct SandboxChan final : renjob::ChanHost {
    uint8_t* ls;
    std::vector<Op> ops;
    uint32_t lsa = 0, eah = 0, eal = 0, size = 0, tagid = 0, mask = 0;
    uint32_t dec = 0xffffff00u;
    explicit SandboxChan(uint8_t* l) : ls(l) {}
    void note(uint8_t k, uint32_t ch, uint32_t v, uint32_t aux = 0) {
        if (ops.size() < 200000) ops.push_back({k, ch, v, aux});
        else abort = true;
    }
    uint32_t hash(uint32_t a, uint32_t n) {
        uint32_t h = 2166136261u;
        for (uint32_t i = 0; i < n; i++) h = (h ^ ls[(a + i) & 0x3ffff]) * 16777619u;
        return h;
    }
    void get(uint32_t l, uint32_t ea, uint32_t n) {
        l &= 0x3ffff;
        if (n > 0x40000 - l) n = 0x40000 - l;
        if (vm::range_mapped(ea, n)) std::memcpy(ls + l, vm::ptr(ea), n);
        else std::memset(ls + l, 0, n);
    }
    void cmd(uint32_t c) {
        uint32_t aux = 0;
        bool list = (c & 0xf) >= 4 && (c & 0xf) <= 6 && c < 0xa0;
        bool put = (c >= 0x20 && c <= 0x36);
        if (c == 0xd0) get(lsa & ~127u, eal & ~127u, 128);
        else if (c == 0xb4 || c == 0xb0 || c == 0xb8) aux = hash(lsa & ~127u, 128);
        else if (!put && !list && (c & 0xf0) == 0x40) get(lsa, eal, size);
        else if (put && !list) aux = hash(lsa, size);
        else if (list) {
            uint32_t cur = lsa & 0x3fff0, l = eal & 0x3fff8;
            for (uint32_t k = size; k >= 8; k -= 8) {
                const uint8_t* e = ls + l;
                uint32_t ts = ((uint32_t(e[2]) << 8) | e[3]) & 0x7fff;
                uint32_t ea = (uint32_t(e[4]) << 24) | (uint32_t(e[5]) << 16) | (uint32_t(e[6]) << 8) | e[7];
                if (put) aux = aux * 31 + hash(cur | (ea & 15), ts);
                else get(cur | (ea & 15), ea, ts);
                cur = (cur + ((ts + 15) & ~15u)) & 0x3fff0;
                l = (l + 8) & 0x3fff8;
            }
        }
        note(3, c, eal, aux);
    }
    uint32_t rd(uint32_t ch) override {
        uint32_t v = 0;
        if (ch == 8) v = dec -= 64;
        else if (ch == 24) v = mask;
        else if (ch == 27) v = 0;
        else if (ch == 13) v = 1;
        note(0, ch, v);
        return v;
    }
    void wr(uint32_t ch, uint32_t v) override {
        switch (ch) {
        case 16: lsa = v; break;
        case 17: eah = v; break;
        case 18: eal = v; break;
        case 19: size = v; break;
        case 20: tagid = v; break;
        case 22: mask = v; break;
        case 21: cmd(v & 0xff); return;
        default: break;
        }
        note(1, ch, v);
    }
    uint32_t cnt(uint32_t ch) override {
        note(2, ch, 1);
        return 1;
    }
};

uint32_t g_every = 0;
std::atomic<uint64_t> g_n{0};
std::atomic<int> g_logged{0};

struct Variant {
    const char* name;
    bool zero_kernel;
    bool pristine_pm;
    bool zero_high;
};

void diff(int job, spu::Context& c) {
    if (!g_every || g_n.fetch_add(1, std::memory_order_relaxed) % g_every) return;
    if (g_logged.load(std::memory_order_relaxed) > 60) return;
    static const Variant vs[] = {{"real", false, false, false}, {"kernel0", true, false, false}, {"pmimg", false, true, false}, {"both", true, true, false}, {"all", true, true, true}};
    std::vector<std::vector<Op>> logs;
    std::vector<std::string> ends;
    for (const Variant& v : vs) {
        std::unique_ptr<uint8_t[]> ls(new uint8_t[spu::LS_SIZE]);
        std::memcpy(ls.get(), c.ls, spu::LS_SIZE);
        if (v.zero_kernel) std::memset(ls.get(), 0, 0xa00);
        if (v.pristine_pm) {
            uint8_t keep_api[0xc0], keep_tag[0x20], keep_cmd[0x100];
            std::memcpy(keep_api, ls.get() + 0x1340, sizeof keep_api);
            std::memcpy(keep_tag, ls.get() + 0x1290, sizeof keep_tag);
            std::memcpy(keep_cmd, ls.get() + 0xc00, sizeof keep_cmd);
            std::memcpy(ls.get() + 0xa00, vm::ptr(0x7a7b00), 0x3000);
            std::memcpy(ls.get() + 0x1340, keep_api, sizeof keep_api);
            std::memcpy(ls.get() + 0x1290, keep_tag, sizeof keep_tag);
            std::memcpy(ls.get() + 0xc00, keep_cmd, sizeof keep_cmd);
        }
        if (v.zero_high) std::memset(ls.get() + 0x3a00, 0, 0x600);
        spu::Context c1;
        std::memcpy(c1.gpr, c.gpr, sizeof c1.gpr);
        c1.ls = ls.get();
        c1.interrupts_enabled = c.interrupts_enabled;
        SandboxChan sb(ls.get());
        Result r = run(job, ls.get(), c1, sb, false, nullptr);
        logs.push_back(std::move(sb.ops));
        char b[64];
        std::snprintf(b, sizeof b, "%s/%05x", r.done ? "done" : "fail", r.fail_pc);
        ends.push_back(b);
    }
    std::string line;
    for (size_t k = 1; k < logs.size(); k++) {
        const auto& a = logs[0];
        const auto& b = logs[k];
        size_t i = 0;
        while (i < a.size() && i < b.size() && a[i].kind == b[i].kind && a[i].ch == b[i].ch && a[i].value == b[i].value && a[i].aux == b[i].aux) i++;
        char s[200];
        if (i == a.size() && i == b.size()) std::snprintf(s, sizeof s, " %s=same(%s)", vs[k].name, ends[k].c_str());
        else if (i < a.size() && i < b.size())
            std::snprintf(s, sizeof s, " %s=op%zu[k%u ch%u v%08x a%08x vs k%u ch%u v%08x a%08x](%s)", vs[k].name, i, a[i].kind, a[i].ch, a[i].value, a[i].aux, b[i].kind, b[i].ch, b[i].value, b[i].aux, ends[k].c_str());
        else std::snprintf(s, sizeof s, " %s=len %zu vs %zu(%s)", vs[k].name, a.size(), b.size(), ends[k].c_str());
        line += s;
    }
    g_logged.fetch_add(1, std::memory_order_relaxed);
    LOG_INFO("wj", "prepdiff %s ops %zu (%s):%s", info(job).name, logs[0].size(), ends[0].c_str(), line.c_str());
}

struct Init {
    Init() {
        if (const char* e = std::getenv("GOWA_WJ_PREPDIFF")) {
            g_every = static_cast<uint32_t>(std::strtoul(e, nullptr, 0));
            g_prep_diff = diff;
        }
    }
} g_init;

}

}
