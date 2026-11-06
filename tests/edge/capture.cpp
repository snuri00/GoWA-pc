#include "capture.h"

#include <cstring>
#include <fstream>
#include <iterator>

namespace edgecap {

bool Capture::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    raw.assign(std::istreambuf_iterator<char>(f), {});
    const uint8_t* p = raw.data();
    const uint8_t* end = p + raw.size();
    auto u32 = [&]() {
        uint32_t v;
        std::memcpy(&v, p, 4);
        p += 4;
        return v;
    };
    if (raw.size() < 16 || u32() != 0x43555053u) return false;
    u32();
    slot = u32();
    insns = u32();
    pc0 = u32();
    srr00 = u32();
    ie0 = u32();
    gpr0 = p;
    p += 2048;
    ls0 = p;
    p += spu::LS_SIZE;
    uint32_t count = u32();
    events.resize(count);
    for (Event& e : events) {
        e.kind = static_cast<int32_t>(u32());
        e.ch = u32();
        e.value = u32();
        e.pc = u32();
        e.srr0 = u32();
        e.ie = p[0];
        e.halted = p[1];
        e.result = p[2];
        p += 4;
        uint32_t lines = u32();
        for (uint32_t i = 0; i < lines; i++) {
            uint32_t l = u32();
            e.lines.push_back({l, p});
            p += 128;
        }
    }
    pc1 = u32();
    srr01 = u32();
    ie1 = u32();
    gpr1 = p;
    p += 2048;
    ls1 = p;
    p += spu::LS_SIZE;
    return p <= end;
}

Machine::Machine(const Capture& cp) : cap(cp) { reset(); }

void Machine::reset() {
    ls.assign(spu::LS_SIZE + 16, 0);
    std::memcpy(ls.data(), cap.ls0, spu::LS_SIZE);
    c = spu::Context{};
    std::memcpy(c.gpr, cap.gpr0, 2048);
    c.pc = cap.pc0;
    c.srr0 = cap.srr00;
    c.interrupts_enabled = cap.ie0 != 0;
    c.ls = ls.data();
    pos = 0;
    n = 0;
    error.clear();
}

const Event* Machine::next(int kind, uint32_t ch, uint32_t value, bool check) {
    if (pos >= cap.events.size()) {
        if (error.empty()) error = "extra channel op kind " + std::to_string(kind) + " ch " + std::to_string(ch);
        c.halted = true;
        return nullptr;
    }
    const Event& e = cap.events[pos++];
    if (e.kind != kind || e.ch != ch || (check && e.value != value)) {
        if (error.empty()) {
            char b[200];
            std::snprintf(b, sizeof b, "event %zu: got kind %d ch %u val 0x%x, recorded kind %d ch %u val 0x%x", pos - 1, kind, ch, value, e.kind, e.ch, e.value);
            error = b;
        }
        c.halted = true;
    }
    for (auto& [l, d] : e.lines) {
        std::memcpy(ls.data() + l, d, 128);
        if (on_line) on_line(l);
    }
    c.pc = e.pc;
    c.srr0 = e.srr0;
    c.interrupts_enabled = e.ie != 0;
    if (e.halted) c.halted = true;
    return &e;
}

uint32_t Machine::read_channel(uint32_t ch) {
    const Event* e = next(0, ch, 0, false);
    uint32_t v = e ? e->value : 0;
    if (on_read) on_read(ch, v);
    return v;
}

void Machine::write_channel(uint32_t ch, uint32_t value) {
    if (on_write) on_write(ch, value);
    if (ch >= 16 && ch <= 21) mfc[ch - 16] = value;
    Dma d;
    bool dma = ch == 21;
    if (dma) {
        d.cmd = value & 0xff;
        d.lsa = mfc[0] & 0x3ffff;
        d.eah = mfc[1];
        d.ea = mfc[2];
        d.size = mfc[3];
        d.tag = mfc[4];
        d.pc = c.pc;
        if (on_dma) on_dma(d, false);
    }
    next(1, ch, value, true);
    if (dma && on_dma) on_dma(d, true);
}

uint32_t Machine::channel_count(uint32_t ch) {
    const Event* e = next(2, ch, 0, false);
    uint32_t v = e ? e->value : 0;
    if (on_count) on_count(ch, v);
    return v;
}

bool Machine::stop_and_signal(uint32_t code) {
    const Event* e = next(3, code, 0, false);
    return e ? e->result != 0 : false;
}

void Machine::halt(uint32_t pc, uint32_t word) { next(4, pc, word, true); }

bool Machine::step() {
    if (done()) return false;
    bool ok = spu::step(c, *this);
    n++;
    if (pos < cap.events.size() && cap.events[pos].kind == 5 && cap.events[pos].value == n) next(5, 0, uint32_t(n), true);
    return ok;
}

void Machine::skip(uint64_t k) {
    uint64_t target = n + k;
    while (pos < cap.events.size() && cap.events[pos].kind == 5 && cap.events[pos].value <= target) {
        const Event& e = cap.events[pos++];
        for (auto& [l, d] : e.lines) {
            std::memcpy(ls.data() + l, d, 128);
            if (on_line) on_line(l);
        }
    }
    n = target;
}

bool Machine::verify_final(std::string& diff) const {
    diff.clear();
    if (!error.empty()) diff += " " + error;
    if (pos != cap.events.size()) diff += " consumed " + std::to_string(pos) + "/" + std::to_string(cap.events.size());
    if (c.pc != cap.pc1) diff += " pc";
    for (int i = 0; i < 128; i++)
        if (std::memcmp(&c.gpr[i], cap.gpr1 + i * 16, 16) != 0) {
            diff += " r" + std::to_string(i);
            break;
        }
    uint32_t bad = 0;
    std::string where;
    for (uint32_t l = 0; l < spu::LS_SIZE; l += 128)
        if (std::memcmp(ls.data() + l, cap.ls1 + l, 128) != 0) {
            if (bad++ < 8) {
                char b[16];
                std::snprintf(b, sizeof b, " %05x", l);
                where += b;
            }
        }
    if (bad) diff += " ls_lines " + std::to_string(bad) + where;
    return diff.empty();
}

}
