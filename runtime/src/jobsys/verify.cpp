#include "verify.h"
#include "mfc.h"

#include "spu/renjob/renjob_chan.h"

#include "runtime/log.h"
#include "runtime/memory.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace jobsys {

uint32_t Recorder::read_channel(uint32_t ch) {
    uint32_t v = t.read_channel(ch);
    ev.push_back({0, ch, v, {}, {}});
    return v;
}

void Recorder::write_channel(uint32_t ch, uint32_t value) {
    if (ch == 16) lsa = value;
    else if (ch == 18) eal = value;
    else if (ch == 19) size = value;
    t.write_channel(ch, value);
    ChanRec r{1, ch, value, {}, {}};
    if (ch == 21) {
        uint32_t cmd = value & 0xff;
        uint32_t cur_ea = eal;
        auto grab = [&](uint32_t a, uint32_t n) {
            a &= 0x3ffff;
            if (n > 0x40000 - a) n = 0x40000 - a;
            r.lines.push_back({a, std::vector<uint8_t>(t.ls + a, t.ls + a + n)});
            r.line_ea.push_back(cur_ea);
        };
        auto out = [&](uint32_t a, uint32_t n) {
            a &= 0x3ffff;
            if (n > 0x40000 - a) n = 0x40000 - a;
            r.outs.push_back({a, std::vector<uint8_t>(t.ls + a, t.ls + a + n)});
            r.out_ea.push_back(cur_ea);
        };
        if (cmd == 0xb4) r.ok = t.atomic_stat == 0;
        bool put = (cmd >= 0x20 && cmd <= 0x26) || (cmd >= 0x30 && cmd <= 0x36) || cmd == 0xa0 || cmd == 0xa1 || cmd == 0xa2;
        auto walk = [&](bool is_out) {
            uint32_t cur = lsa & 0x3fff0, list = eal & 0x3fff8;
            for (uint32_t n = size; n >= 8; n -= 8) {
                uint8_t* e = t.ls + list;
                uint32_t ts = ((uint32_t(e[2]) << 8) | e[3]) & 0x7fff;
                uint32_t ea;
                std::memcpy(&ea, e + 4, 4);
                ea = __builtin_bswap32(ea);
                cur_ea = ea;
                if (is_out) out(cur | (ea & 15), ts);
                else grab(cur | (ea & 15), ts);
                cur = (cur + ((ts + 15) & ~15u)) & 0x3fff0;
                list = (list + 8) & 0x3fff8;
            }
        };
        if (put && ((cmd & 0xf) >= 4 && (cmd & 0xf) <= 6)) {
            walk(true);
        } else if (put) {
            out(lsa, cmd >= 0xa0 ? 4 : size);
        } else if (cmd == 0xb4 || cmd == 0xb0 || cmd == 0xb8) {
            cur_ea = eal & ~127u;
            out(lsa & ~127u, 128);
        }
        if (cmd == 0x40 || cmd == 0x41 || cmd == 0x42) {
            grab(lsa, size);
        } else if (cmd == 0xd0) {
            cur_ea = eal & ~127u;
            grab(lsa & ~127u, 128);
        } else if (cmd == 0x44 || cmd == 0x45 || cmd == 0x46) {
            walk(false);
        }
    }
    ev.push_back(std::move(r));
}

uint32_t Recorder::channel_count(uint32_t ch) {
    uint32_t v = t.channel_count(ch);
    ev.push_back({2, ch, v, {}, {}});
    return v;
}

bool Recorder::stop_and_signal(uint32_t code) {
    bool r = t.stop_and_signal(code);
    ev.push_back({3, code, 0, {}, {}});
    return r;
}

void Recorder::halt(uint32_t pc, uint32_t word) {
    t.halt(pc, word);
    ev.push_back({4, pc, word, {}, {}});
}

namespace {

struct Replayer : spu::Host {
    spu::SpuThread& t;
    const std::vector<ChanRec>& ev;
    size_t pos = 0;
    std::string error;
    uint32_t puts = 0, put_bytes = 0;
    uint32_t lsa = 0, eal = 0, size = 0;
    Replayer(spu::SpuThread& th, const std::vector<ChanRec>& e) : t(th), ev(e) {}
    const ChanRec* next(int kind, uint32_t ch, uint32_t value, bool check) {
        if (pos >= ev.size()) {
            if (error.empty()) error = "extra channel op kind " + std::to_string(kind) + " ch " + std::to_string(ch) + " pc " + std::to_string(t.ctx.pc);
            t.stop_flag.store(true);
            t.ctx.halted = true;
            return nullptr;
        }
        const ChanRec& e = ev[pos++];
        if (e.kind != kind || e.ch != ch || (check && e.value != value)) {
            if (error.empty()) {
                char b[200];
                std::snprintf(b, sizeof b, "op %zu: got kind %d ch %u val 0x%x, ref kind %d ch %u val 0x%x at pc 0x%05x", pos - 1, kind, ch, value, e.kind, e.ch, e.value, t.ctx.pc);
                error = b;
            }
            t.stop_flag.store(true);
            t.ctx.halted = true;
            return nullptr;
        }
        return &e;
    }
    void compare_out(uint32_t a, uint32_t n, const uint8_t* expect) {
        a &= 0x3ffff;
        if (n > 0x40000 - a) n = 0x40000 - a;
        puts++;
        put_bytes += n;
        if (std::memcmp(t.ls + a, expect, n) != 0 && error.empty()) {
            uint32_t k = 0;
            while (k < n && t.ls[a + k] == expect[k]) k++;
            char b[160];
            std::snprintf(b, sizeof b, "output differs: lsa 0x%05x size 0x%x first diff at +0x%x (0x%02x vs ref 0x%02x) ea 0x%08x pc 0x%05x", a, n, k, t.ls[a + k], expect[k], eal, t.ctx.pc);
            error = b;
        }
    }
    uint32_t read_channel(uint32_t ch) override {
        const ChanRec* e = next(0, ch, 0, false);
        return e ? e->value : 0;
    }
    void write_channel(uint32_t ch, uint32_t value) override {
        if (ch == 16) lsa = value;
        else if (ch == 18) eal = value;
        else if (ch == 19) size = value;
        const ChanRec* e = next(1, ch, value, true);
        if (ch != 21 || !e) return;
        for (auto& [a, data] : e->outs) compare_out(a, static_cast<uint32_t>(data.size()), data.data());
        for (auto& [a, data] : e->lines) {
            std::memcpy(t.ls + a, data.data(), data.size());
            t.mark_ls(a, static_cast<uint32_t>(data.size()));
        }
    }
    uint32_t channel_count(uint32_t ch) override {
        const ChanRec* e = next(2, ch, 0, false);
        return e ? e->value : 0;
    }
    bool stop_and_signal(uint32_t code) override {
        next(3, code, 0, false);
        return false;
    }
    void halt(uint32_t pc, uint32_t word) override { next(4, pc, word, true); }
};

struct SpuHostChan final : renjob::ChanHost {
    spu::Host& h;
    spu::SpuThread& t;
    SpuHostChan(spu::Host& host, spu::SpuThread& th) : h(host), t(th) {}
    uint32_t rd(uint32_t ch) override {
        uint32_t v = h.read_channel(ch);
        abort = t.ctx.halted;
        return v;
    }
    void wr(uint32_t ch, uint32_t v) override {
        h.write_channel(ch, v);
        abort = t.ctx.halted;
    }
    uint32_t cnt(uint32_t ch) override { return h.channel_count(ch); }
};

struct Inputs {
    std::unordered_map<uint32_t, std::vector<uint8_t>> reads;
    std::map<uint32_t, uint8_t> writes;
    std::vector<uint8_t> llc_ok;
};

Inputs collect(const std::vector<ChanRec>& ev) {
    Inputs in;
    for (const ChanRec& e : ev) {
        for (size_t i = 0; i < e.lines.size() && i < e.line_ea.size(); i++) {
            const auto& d = e.lines[i].second;
            for (uint32_t k = 0; k < d.size(); k++) in.reads[e.line_ea[i] + k].push_back(d[k]);
        }
        if (e.kind == 1 && e.ch == 21 && (e.value & 0xff) == 0xb4) in.llc_ok.push_back(e.ok ? 1 : 0);
        if (!e.ok) continue;
        for (size_t i = 0; i < e.outs.size() && i < e.out_ea.size(); i++) {
            const auto& d = e.outs[i].second;
            for (uint32_t k = 0; k < d.size(); k++) in.writes[e.out_ea[i] + k] = d[k];
        }
    }
    return in;
}

struct SbxReads {
    const Inputs& in;
    std::unordered_map<uint32_t, uint32_t> rpos;
    std::map<uint32_t, uint8_t> writes;
    uint32_t live_bytes = 0;
    size_t llc_pos = 0;
    explicit SbxReads(const Inputs& i) : in(i) {}
    uint8_t byte(uint32_t ea) {
        auto i = in.reads.find(ea);
        if (i != in.reads.end()) {
            uint32_t k = rpos[ea]++;
            return i->second[k < i->second.size() ? k : i->second.size() - 1];
        }
        auto w = writes.find(ea);
        if (w != writes.end()) return w->second;
        live_bytes++;
        return vm::is_mapped(ea) ? vm::ptr(ea)[0] : 0;
    }
    bool next_llc() {
        bool ok = llc_pos < in.llc_ok.size() ? in.llc_ok[llc_pos] != 0 : true;
        llc_pos++;
        return ok;
    }
};

struct SandboxHost final : renjob::ChanHost {
    spu::SpuThread& t;
    SbxReads r;
    SandboxHost(spu::SpuThread& th, const Inputs& in) : t(th), r(in) {}
    void load(uint32_t lsa, uint32_t ea, uint32_t n) {
        for (uint32_t k = 0; k < n; k++) t.ls[(lsa + k) & 0x3FFFF] = r.byte(ea + k);
    }
    void store(uint32_t lsa, uint32_t ea, uint32_t n) {
        for (uint32_t k = 0; k < n; k++) r.writes[ea + k] = t.ls[(lsa + k) & 0x3FFFF];
    }
    uint32_t rd(uint32_t ch) override {
        switch (ch) {
        case 13: return t.ctx.interrupts_enabled ? 1 : 0;
        case 24: t.tag_stat_count = false; return t.tag_stat;
        case 27: t.atomic_stat_count = false; return t.atomic_stat;
        case 8: return 0;
        default: abort = true; return 0;
        }
    }
    uint32_t cnt(uint32_t ch) override {
        switch (ch) {
        case 21: return 16;
        case 23: return 1;
        case 24: return t.tag_stat_count;
        case 27: return t.atomic_stat_count;
        default: abort = true; return 0;
        }
    }
    void wr(uint32_t ch, uint32_t v) override {
        switch (ch) {
        case 16: t.mfc_lsa = v; return;
        case 17: return;
        case 18: t.mfc_eal = v; return;
        case 19: t.mfc_size = v > 0xFFFF ? 0xFFFF : v; return;
        case 20: t.mfc_tag = v & 0x1F; return;
        case 22: t.tag_mask = v; return;
        case 23:
            t.tag_stat = t.tag_mask;
            if (v == 1 && t.tag_mask == 0) return;
            t.tag_stat_count = true;
            return;
        case 21: break;
        default: abort = true; return;
        }
        uint32_t cmd = v & 0xFF, lsa = t.mfc_lsa & 0x3FFFF, ea = t.mfc_eal, size = t.mfc_size;
        switch (cmd) {
        case 0x40: case 0x41: case 0x42: load(lsa, ea, size); return;
        case 0x20: case 0x21: case 0x22: store(lsa, ea, size); return;
        case 0x44: case 0x45: case 0x46: case 0x24: case 0x25: case 0x26: {
            uint32_t cur = lsa & 0x3FFF0, list = ea & 0x3FFF8;
            for (uint32_t n = size; n >= 8; n -= 8) {
                const uint8_t* e = t.ls + list;
                uint32_t ts = ((uint32_t(e[2]) << 8) | e[3]) & 0x7FFF;
                uint32_t a;
                std::memcpy(&a, e + 4, 4);
                a = __builtin_bswap32(a);
                if (cmd < 0x40) store(cur | (a & 15), a, ts);
                else load(cur | (a & 15), a, ts);
                cur = (cur + ((ts + 15) & ~15u)) & 0x3FFF0;
                list = (list + 8) & 0x3FFF8;
            }
            return;
        }
        case 0xD0:
            load(lsa & 0x3FF80, ea & ~127u, 128);
            t.atomic_stat = 4;
            t.atomic_stat_count = true;
            return;
        case 0xB4: {
            bool ok = r.next_llc();
            if (ok) store(lsa & 0x3FF80, ea & ~127u, 128);
            t.atomic_stat = ok ? 0 : 1;
            t.atomic_stat_count = true;
            return;
        }
        case 0xB0: case 0xB8:
            store(lsa & 0x3FF80, ea & ~127u, 128);
            if (cmd == 0xB0) {
                t.atomic_stat = 2;
                t.atomic_stat_count = true;
            }
            return;
        case 0xC0: case 0xC8: case 0xCC: case 0x80: case 0x81: return;
        default: abort = true; return;
        }
    }
};

struct SandboxMem final : Mem {
    SbxReads r;
    explicit SandboxMem(const Inputs& in) : r(in) {}
    void get(void* dst, uint32_t ea, uint32_t size) override {
        auto* d = static_cast<uint8_t*>(dst);
        for (uint32_t k = 0; k < size; k++) d[k] = r.byte(ea + k);
    }
    void put(uint32_t ea, const void* src, uint32_t size) override {
        auto* s = static_cast<const uint8_t*>(src);
        for (uint32_t k = 0; k < size; k++) r.writes[ea + k] = s[k];
    }
    uint64_t getllar(uint32_t ea, void* line) override {
        get(line, ea & ~127u, 128);
        return 1;
    }
    bool putllc(uint32_t ea, uint64_t, const void*, const void* line) override {
        bool ok = r.next_llc();
        if (ok) put(ea & ~127u, line, 128);
        return ok;
    }
    void putlluc(uint32_t ea, const void* line) override { put(ea & ~127u, line, 128); }
    uint32_t read32(uint32_t ea) override {
        uint8_t b[4];
        get(b, ea, 4);
        return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
    }
};

struct VStats {
    std::mutex m;
    std::map<std::pair<uint32_t, uint32_t>, std::array<uint64_t, 4>> by;
};
VStats g_v;

void note(uint32_t mod, uint32_t kind, int k) {
    std::lock_guard lk(g_v.m);
    g_v.by[{mod, kind}][k]++;
}

std::atomic<uint64_t> g_seq{0};

void compare_maps(const std::map<uint32_t, uint8_t>& ref, const std::map<uint32_t, uint8_t>& got, verify::Outcome& o) {
    for (auto& [a, v] : ref) {
        auto it = got.find(a);
        if (it == got.end() || it->second != v) {
            if (!o.diff_bytes) o.first_ea = a;
            o.diff_bytes++;
        }
    }
    for (auto& [a, v] : got) {
        if (!ref.count(a)) {
            if (!o.diff_bytes && !o.extra_bytes) o.first_ea = a;
            o.extra_bytes++;
        }
    }
    o.same = !o.diff_bytes && !o.extra_bytes;
}

spu::SpuThread& shadow_of(Executor& x) {
    if (!x.shadow) x.shadow = make_spu(x.index, "jobsys verify");
    return *x.shadow;
}

void load_start(Executor& x, spu::SpuThread& s, const JobDesc& j) {
    std::memcpy(s.ls, x.start_ls.data(), spu::LS_SIZE);
    s.mark_all_ls();
    std::memcpy(s.ctx.gpr, x.start_gpr, sizeof s.ctx.gpr);
    reset_regs(s, j);
}

}

namespace verify {

void poison(spu::SpuThread& t, uint64_t seed, uint32_t mask) {
    uint64_t x = seed * 0x9e3779b97f4a7c15ull + 1;
    auto rnd = [&] {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return x;
    };
    for (uint32_t a = 0x17580; a < 0x40000; a += 8) {
        uint64_t v = rnd();
        bool stack = a >= 0x3f000;
        if ((stack && (mask & 4)) || (!stack && (mask & 2))) std::memcpy(t.ls + a, &v, 8);
    }
    static const uint32_t ranges[][2] = {{0x17580, 0x19300}, {0x19300, 0x1a000}, {0x1a000, 0x38a00}, {0x38a00, 0x391a0}, {0x391a0, 0x3b380}, {0x3b380, 0x3f000}};
    for (int k = 0; k < 6; k++) {
        if (!(mask & (0x100u << k))) continue;
        for (uint32_t a = ranges[k][0]; a < ranges[k][1]; a += 8) {
            uint64_t v = rnd();
            std::memcpy(t.ls + a, &v, 8);
        }
    }
    if (mask & 8) {
        for (uint32_t a = 0; a < 0xa00; a += 8) {
            uint64_t v = rnd();
            std::memcpy(t.ls + a, &v, 8);
        }
    }
    t.mark_all_ls();
    if (mask & 1) {
        for (int k = 7; k < 128; k++) {
            t.ctx.gpr[k].u64[0] = rnd();
            t.ctx.gpr[k].u64[1] = rnd();
        }
    }
}

bool want(uint32_t module_ea) {
    (void)module_ea;
    uint32_t n = cfg().verify;
    return n && g_seq.fetch_add(1, std::memory_order_relaxed) % n == 0;
}

bool run_verified(Executor& x, const JobDesc& j, Tier tier) {
    const ModuleInfo* m = find_module(j.module_ea);
    spu::SpuThread& t = *x.t;
    prepare_job(x, j);
    x.start_ls.assign(t.ls, t.ls + spu::LS_SIZE);
    std::memcpy(x.start_gpr, t.ctx.gpr, sizeof t.ctx.gpr);
    if (!x.rec) x.rec = std::make_unique<Recorder>(t);
    x.rec->ev.clear();
    emulate(t, x.rec.get());
    x.last = Tier::Executor;
    const std::vector<ChanRec>& ev = x.rec->ev;
    Inputs in = collect(ev);
    spu::SpuThread& s = shadow_of(x);
    static std::atomic<int> logged{0};
    auto chan_diff = [&](uint32_t mask, bool whole) {
        load_start(x, s, j);
        if (mask) poison(s, j.seq * 31 + 7, mask);
        Replayer rp(s, ev);
        s.stop_flag.store(false);
        bool resumed = false;
        if (whole && m && m->whole) {
            SpuHostChan ch(rp, s);
            WholeResult r = m->whole(s.ls, s.ctx, ch);
            if (r.done) s.ctx.pc = kJobReturn;
            else if (!s.ctx.halted && rp.error.empty()) {
                s.ctx.pc = r.fail_pc;
                emulate(s, &rp);
                resumed = true;
            }
        } else {
            emulate(s, &rp);
        }
        s.stop_flag.store(false);
        if (rp.error.empty() && rp.pos != ev.size()) rp.error = "consumed " + std::to_string(rp.pos) + " of " + std::to_string(ev.size()) + " channel ops";
        bool bad = !rp.error.empty();
        note(j.module_ea, (whole ? 0x1000u : 0u) | mask, bad ? 1 : 0);
        if (bad && logged.fetch_add(1) < 40)
            LOG_ERROR("jobsys", "verify MISMATCH %s %s mask 0x%x cmd 0x%08x: %s", m ? m->name : "?", whole ? "whole" : "executor", mask, j.cmd_ea, rp.error.c_str());
        else if (!bad && logged.load() < 40 && resumed) {
            logged.fetch_add(1);
            LOG_INFO("jobsys", "verify ok %s (resumed) cmd 0x%08x", m ? m->name : "?", j.cmd_ea);
        }
    };
    if (tier == Tier::Whole && m && m->whole) {
        chan_diff(0, true);
        for (uint32_t mask : cfg().sandbox) {
            load_start(x, s, j);
            if (mask) poison(s, j.seq * 17 + 3, mask);
            SandboxHost h(s, in);
            WholeResult r = m->whole(s.ls, s.ctx, h);
            Outcome o;
            if (!r.done || h.abort) {
                note(j.module_ea, 0x2000u | mask, 2);
                continue;
            }
            compare_maps(in.writes, h.r.writes, o);
            note(j.module_ea, 0x2000u | mask, o.same ? 0 : 1);
            if (!o.same && logged.fetch_add(1) < 40)
                LOG_ERROR("jobsys", "sandbox DIFFERS %s mask 0x%x cmd 0x%08x: %u ref bytes differ, %u extra, first 0x%08x", m->name, mask, j.cmd_ea, o.diff_bytes, o.extra_bytes, o.first_ea);
        }
    } else if (tier == Tier::Semantic && m && m->semantic) {
        SandboxMem sm(in);
        JobCtx c;
        c.worker = x.index;
        c.job = &j;
        c.mem = &sm;
        c.exec = &x;
        load_start(x, s, j);
        c.ls = s.ls;
        c.regs = &s.ctx;
        Outcome o;
        if (!m->semantic(j, c)) {
            note(j.module_ea, 0x4000u, 2);
        } else {
            compare_maps(in.writes, sm.r.writes, o);
            note(j.module_ea, 0x4000u, o.same ? 0 : 1);
            if (!o.same && logged.fetch_add(1) < 40)
                LOG_ERROR("jobsys", "semantic DIFFERS %s cmd 0x%08x: %u ref bytes differ, %u extra, first 0x%08x", m->name, j.cmd_ea, o.diff_bytes, o.extra_bytes, o.first_ea);
        }
    } else {
        const std::vector<uint32_t>& masks = cfg().vpoison;
        if (masks.empty()) chan_diff(0, false);
        for (uint32_t mask : masks) chan_diff(mask, false);
    }
    return true;
}

Outcome check(const JobDesc& job, JobCtx& ctx, Tier candidate) {
    Outcome o;
    Executor& x = ctx.exec ? *ctx.exec : thread_executor(ctx.worker);
    o.ran = run_verified(x, job, candidate);
    return o;
}

void report() {
    std::lock_guard lk(g_v.m);
    if (g_v.by.empty()) return;
    std::string s;
    for (auto& [k, v] : g_v.by) {
        const ModuleInfo* m = find_module(k.first);
        const char* kind = (k.second & 0x4000) ? "sem" : (k.second & 0x2000) ? "sbx" : (k.second & 0x1000) ? "whole" : "exec";
        char b[128];
        std::snprintf(b, sizeof b, " %s/%s/%x ok %llu bad %llu skip %llu;", m ? m->name : "?", kind, k.second & 0xfff, (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2]);
        s += b;
    }
    LOG_INFO("jobsys", "verify:%s", s.c_str());
}

}

}
