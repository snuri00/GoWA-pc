#include "core.h"

#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/reservation.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <immintrin.h>

namespace jobsys {

namespace {

std::vector<uint32_t> parse_hex(const char* e) {
    std::vector<uint32_t> v;
    if (!e) return v;
    std::string s = e;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        if (end > pos) v.push_back(static_cast<uint32_t>(std::strtoul(s.substr(pos, end - pos).c_str(), nullptr, 16)));
        pos = end + 1;
    }
    return v;
}

Tier parse_tier(const std::string& s) {
    if (s == "semantic" || s == "s" || s == "0") return Tier::Semantic;
    if (s == "whole" || s == "w" || s == "1") return Tier::Whole;
    if (s == "executor" || s == "exec" || s == "e" || s == "2") return Tier::Executor;
    if (s == "wws" || s == "off" || s == "3") return Tier::Wws;
    return Tier::Auto;
}

bool env_on(const char* name, bool def) {
    const char* e = std::getenv(name);
    if (!e || !*e) return def;
    return e[0] != '0';
}

}

const Config& cfg() {
    static const Config c = [] {
        Config c;
        bool legacy = env_on("GOWA_PPU_JOBS", false);
        c.on = env_on("GOWA_JOBSYS", kDefaultOn) || legacy;
        c.mods = parse_hex(std::getenv("GOWA_JOBSYS_MODS"));
        if (c.mods.empty() && legacy) c.mods = parse_hex(std::getenv("GOWA_PPU_JOBS_MODS"));
        c.all_mods = c.mods.empty();
        if (const char* t = std::getenv("GOWA_JOBSYS_TIER")) {
            std::string s = t;
            size_t pos = 0;
            while (pos < s.size()) {
                size_t end = s.find(',', pos);
                if (end == std::string::npos) end = s.size();
                std::string item = s.substr(pos, end - pos);
                size_t colon = item.find(':');
                if (colon == std::string::npos) c.tier = parse_tier(item);
                else c.tier_mod.push_back({static_cast<uint32_t>(std::strtoul(item.substr(0, colon).c_str(), nullptr, 16)), parse_tier(item.substr(colon + 1))});
                pos = end + 1;
            }
        }
        const char* v = std::getenv("GOWA_JOBSYS_VERIFY");
        if (!v && legacy) v = std::getenv("GOWA_PPU_JOBS_VERIFY");
        if (v) c.verify = static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
        const char* w = std::getenv("GOWA_JOBSYS_WORKERS");
        if (!w && legacy) w = std::getenv("GOWA_PPU_JOBS_THREADS");
        if (w) c.workers = std::clamp(std::atoi(w), 0, 16);
        const char* cp = std::getenv("GOWA_JOBSYS_CPUS");
        if (!cp && legacy) cp = std::getenv("GOWA_PPU_JOBS_CPUS");
        for (uint32_t x : parse_hex(cp)) c.cpus.push_back(static_cast<int>(x));
        const char* p = std::getenv("GOWA_JOBSYS_PRIO");
        if (!p && legacy) p = std::getenv("GOWA_PPU_JOBS_PRIO");
        if (p) c.prio = std::atoi(p);
        c.help = env_on("GOWA_JOBSYS_HELP", env_on("GOWA_PPU_JOBS_HELP", true));
        c.help_sites = env_on("GOWA_JOBSYS_HELP_SITES", true);
        c.donate = env_on("GOWA_JOBSYS_DONATE", false);
        if (const char* d = std::getenv("GOWA_JOBSYS_DONOR_SPU")) c.donor_spu = std::atoi(d);
        if (const char* s = std::getenv("GOWA_JOBSYS_SBX")) c.sandbox = parse_hex(s);
        if (const char* s = std::getenv("GOWA_JOBSYS_VPOISON")) c.vpoison = parse_hex(s);
        if (const char* s = std::getenv("GOWA_JOBSYS_POISON")) c.poison = static_cast<uint32_t>(std::strtoul(s, nullptr, 16));
        c.stats = env_on("GOWA_JOBSYS_STATS", true);
        c.fast_mfc = env_on("GOWA_JOBSYS_FASTMFC", true);
        c.ordered = env_on("GOWA_JOBSYS_ORDERED", true);
        c.relax_sleep_us = std::getenv("GOWA_JOBSYS_RELAX_US") ? std::atoi(std::getenv("GOWA_JOBSYS_RELAX_US")) : 40;
        return c;
    }();
    return c;
}

bool enabled() { return cfg().on; }
uint32_t verify_every() { return cfg().verify; }

const char* tier_name(Tier t) {
    switch (t) {
    case Tier::Semantic: return "semantic";
    case Tier::Whole: return "whole";
    case Tier::Executor: return "executor";
    case Tier::Wws: return "wws";
    default: return "auto";
    }
}

const char* decode_name(Decode d) {
    switch (d) {
    case Decode::Ok: return "ok";
    case Decode::BadHeader: return "header";
    case Decode::BadBlock: return "block";
    case Decode::BadOp: return "op";
    case Decode::BadLoad: return "load";
    case Decode::NoRun: return "no-run";
    case Decode::NoCode: return "no-code";
    case Decode::TooMany: return "too-many";
    }
    return "?";
}

Decode decode(uint64_t header, uint32_t list_obj, JobDesc& j) {
    j = JobDesc{};
    j.header = header;
    j.cmd_ea = static_cast<uint32_t>(header);
    j.cmd_size = static_cast<uint32_t>(header >> 48);
    j.list_obj = list_obj;
    if (((header >> 32) & 0xffff) != 1 || !vm::range_mapped(list_obj, 0x40)) return Decode::BadHeader;
    j.list_hdr = vm::read32(list_obj);
    j.spurs = vm::read32(list_obj + 4);
    j.wid = vm::read32(list_obj + 8);
    if (!j.spurs || !vm::range_mapped(j.spurs, 0x80) || j.wid >= 32) return Decode::BadHeader;
    if (j.cmd_size < 0x10 || j.cmd_size > kMaxCmd || (j.cmd_ea & 15) || !vm::range_mapped(j.cmd_ea, j.cmd_size)) return Decode::BadBlock;
    std::memcpy(j.cmd, vm::ptr(j.cmd_ea), j.cmd_size);
    for (uint32_t off = 0; off + 8 <= j.cmd_size; off += 8) {
        uint32_t hi = uint32_t(j.cmd[off]) << 24 | uint32_t(j.cmd[off + 1]) << 16 | uint32_t(j.cmd[off + 2]) << 8 | j.cmd[off + 3];
        uint32_t lo = uint32_t(j.cmd[off + 4]) << 24 | uint32_t(j.cmd[off + 5]) << 16 | uint32_t(j.cmd[off + 6]) << 8 | j.cmd[off + 7];
        if (!hi && !lo) continue;
        uint32_t op = (hi >> 9) & 7, set = (hi >> 5) & 15, buf = hi & 31;
        switch (op) {
        case 0: break;
        case 1: {
            BufferSet& s = j.sets[set];
            s.first_page = static_cast<uint8_t>(lo >> 24);
            s.pages = static_cast<uint8_t>(lo >> 16);
            s.buffers = static_cast<uint8_t>(lo >> 8);
            s.reserved = true;
            if (!s.pages || !s.buffers || (uint32_t(s.first_page) + uint32_t(s.pages) * s.buffers) * 0x400u > 0x40000u) return Decode::BadOp;
            break;
        }
        case 2: {
            if (j.nloads >= kMaxLoads) return Decode::TooMany;
            if (!j.sets[set].reserved || buf >= j.sets[set].buffers) return Decode::BadLoad;
            Load& l = j.loads[j.nloads++];
            l.set = static_cast<uint8_t>(set);
            l.buf = static_cast<uint8_t>(buf);
            l.list = (hi & 0x10000) != 0;
            l.size = (hi >> 18) << 4;
            l.ea = lo;
            l.word = hi;
            if (l.size && ((l.ea & 15) && !l.list)) return Decode::BadLoad;
            if (l.size && !vm::range_mapped(l.ea, l.list ? 8 : l.size)) return Decode::BadLoad;
            if (l.size && (l.list ? 0x40000u : l.size) > uint32_t(j.sets[set].pages) * 0x400u) return Decode::BadLoad;
            if (!j.module_ea && set == 0 && !l.list && l.size >= 0x30 && vm::read32(l.ea + 0x20) == 0xc0dec0de && j.sets[0].first_page == 0x10) {
                l.code = true;
                j.module_ea = l.ea;
                j.module_size = l.size;
            }
            break;
        }
        case 3: j.commit_mask |= lo; break;
        case 4:
            if (j.ndeps >= kMaxDeps) return Decode::TooMany;
            if (!lo || !vm::range_mapped(lo & ~127u, 128)) return Decode::BadOp;
            j.deps[j.ndeps++] = lo;
            break;
        case 5:
            if (!j.module_ea) return Decode::NoCode;
            j.param_off = (off + 8 + 15) & ~15u;
            if (j.param_off > j.cmd_size) j.param_off = j.cmd_size;
            j.entry = 0x4000 + vm::read32(j.module_ea + 0x10);
            if (j.entry >= 0x4000 + j.module_size) return Decode::NoCode;
            return Decode::Ok;
        default: return Decode::BadOp;
        }
    }
    return Decode::NoRun;
}

namespace {

struct Registry {
    std::mutex m;
    ModuleInfo mods[64];
    std::atomic<uint32_t> n{0};
};

Registry& registry() {
    static Registry r;
    return r;
}

}

void register_module(const ModuleInfo& m) {
    Registry& r = registry();
    std::lock_guard lk(r.m);
    uint32_t n = r.n.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < n; i++) {
        if (r.mods[i].ea != m.ea) continue;
        ModuleInfo& d = r.mods[i];
        if (m.semantic) d.semantic = m.semantic;
        if (m.whole) {
            d.whole = m.whole;
            d.whole_entry = m.whole_entry;
        }
        if (m.state_hi > m.state_lo) {
            d.state_lo = m.state_lo;
            d.state_hi = m.state_hi;
        }
        d.flags |= m.flags;
        if (m.name && *m.name) d.name = m.name;
        return;
    }
    if (n >= 64) return;
    r.mods[n] = m;
    r.n.store(n + 1, std::memory_order_release);
}

const ModuleInfo* find_module(uint32_t ea) {
    Registry& r = registry();
    uint32_t n = r.n.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; i++)
        if (r.mods[i].ea == ea) return &r.mods[i];
    return nullptr;
}

void for_each_module(void (*fn)(const ModuleInfo&, void*), void* user) {
    Registry& r = registry();
    uint32_t n = r.n.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; i++) fn(r.mods[i], user);
}

bool module_enabled(uint32_t ea) {
    const Config& c = cfg();
    if (!c.on) return false;
    if (!c.all_mods) return std::find(c.mods.begin(), c.mods.end(), ea) != c.mods.end();
    const ModuleInfo* m = find_module(ea);
    return m && !(m->flags & kOptIn);
}

Tier tier_for(uint32_t ea) {
    const Config& c = cfg();
    Tier want = c.tier;
    for (auto& [m, t] : c.tier_mod)
        if (m == ea) want = t;
    const ModuleInfo* m = find_module(ea);
    Tier best = Tier::Executor;
    if (m && m->whole) best = Tier::Whole;
    if (m && m->semantic) best = Tier::Semantic;
    if (m && (m->flags & kNoExecutor) && best == Tier::Executor) best = Tier::Wws;
    if (want == Tier::Auto) return best;
    if (want == Tier::Wws) return Tier::Wws;
    if (want == Tier::Semantic && !(m && m->semantic)) return best;
    if (want == Tier::Whole && !(m && m->whole)) return best == Tier::Semantic ? Tier::Whole : best;
    if (want == Tier::Executor && m && (m->flags & kNoExecutor)) return Tier::Wws;
    return want;
}

void update_line(uint32_t ea, bool (*fn)(uint8_t* line, uint32_t off, void* user), void* user) {
    uint32_t line = ea & ~127u;
    alignas(16) uint8_t old_data[128], new_data[128];
    uint32_t spins = 0;
    for (;;) {
        uint64_t rtime = rsv::snapshot(line, old_data);
        std::memcpy(new_data, old_data, 128);
        if (!fn(new_data, ea & 127u, user)) return;
        if (rsv::commit_if(line, rtime, old_data, new_data, 0, 128)) return;
        relax_spin(spins);
    }
}

void kick_workload(uint32_t spurs, uint32_t wid, uint8_t value) {
    struct K {
        uint8_t value;
    } k{value};
    update_line(spurs + (wid < 16 ? 0 : 0x10) + (wid & 15), [](uint8_t* l, uint32_t off, void* u) {
        l[off] = static_cast<K*>(u)->value;
        return true;
    }, &k);
}

void dep_decrement(uint32_t ea, uint32_t spurs) {
    struct Kick {
        uint32_t wid = 0;
        uint8_t value = 0;
        bool fire = false;
    } k;
    update_line(ea, [](uint8_t* l, uint32_t off, void* u) {
        auto* k = static_cast<Kick*>(u);
        uint16_t cnt = uint16_t((l[off] << 8) | l[off + 1]);
        uint16_t nc = uint16_t(cnt - 1);
        l[off] = uint8_t(nc >> 8);
        l[off + 1] = uint8_t(nc);
        k->fire = cnt == 1;
        k->wid = l[off + 2];
        k->value = l[off + 3];
        return true;
    }, &k);
    if (k.fire && k.value) kick_workload(spurs, k.wid, k.value);
}

thread_local bool t_donor_prio = false;

void set_relax_hard(bool on) {
    t_donor_prio = on;
    rsv::set_hard_relax(on);
}

void relax_spin(uint32_t& n) {
    n++;
    if (n < 64) _mm_pause();
    else if (n < 512) SwitchToThread();
    else if (t_donor_prio) Sleep(1);
    else if (n < 4096) SwitchToThread();
    else Sleep(1);
}

void JobCtx::relax() { relax_spin(spins); }

void JobCtx::dep_decrement(uint32_t ea) { jobsys::dep_decrement(ea, job ? job->spurs : 0); }

namespace {

struct LiveMem final : Mem {
    void get(void* dst, uint32_t ea, uint32_t size) override {
        if (!size) return;
        if (vm::is_immutable(ea, size)) {
            std::memcpy(dst, vm::ptr(ea), size);
            return;
        }
        uint32_t lo = 0xffffffffu, hi = 0;
        rsv::copy_range_if_changed(ea, static_cast<uint8_t*>(dst), size, lo, hi);
    }
    void put(uint32_t ea, const void* src, uint32_t size) override { rsv::store_range(ea, static_cast<const uint8_t*>(src), size); }
    uint64_t getllar(uint32_t ea, void* line) override { return rsv::snapshot(ea & ~127u, static_cast<uint8_t*>(line)); }
    bool putllc(uint32_t ea, uint64_t rtime, const void* expect, const void* line) override {
        return rsv::commit_if(ea & ~127u, rtime, static_cast<const uint8_t*>(expect), static_cast<const uint8_t*>(line), 0, 128);
    }
    void putlluc(uint32_t ea, const void* line) override { rsv::store_unconditional(ea & ~127u, static_cast<const uint8_t*>(line)); }
    uint32_t read32(uint32_t ea) override {
        std::atomic_thread_fence(std::memory_order_acquire);
        return vm::read32(ea);
    }
};

}

Mem& live_mem() {
    static LiveMem m;
    return m;
}

}
