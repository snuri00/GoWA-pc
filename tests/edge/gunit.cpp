#include "glue_reg.h"
#include "natreg.h"
#include "unit.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

enum OpKind { kCmd, kWait, kDec };

struct Op {
    OpKind kind = kCmd;
    uint32_t cmd = 0, lsa = 0, eah = 0, ea = 0, size = 0, tag = 0;
    uint32_t mask = 0, update = 0, status = 0, value = 0, pc = 0;
    int count = 1;
    bool has_status = false;
    bool has_mask = false;
    std::vector<uint8_t> payload;
};

struct Line {
    size_t op = 0;
    uint32_t lsa = 0;
    uint8_t data[128];
};

struct Read {
    size_t op = 0;
    uint32_t ch = 0, value = 0, pc = 0;
};

struct Recording {
    std::vector<Op> ops;
    std::vector<Line> lines;
    std::vector<Read> reads;
    int stray_writes = 0;
};

bool is_atomic(uint32_t cmd) { return cmd == 0xd0 || cmd == 0xb4 || cmd == 0xb0 || cmd == 0xb8; }
bool is_list(uint32_t cmd) { return (cmd & 0xf) == 4 || (cmd & 0xf) == 5 || (cmd & 0xf) == 6; }
bool is_put(uint32_t cmd) { return (cmd & 0xf0) == 0x20 || (cmd & 0xf0) == 0x30 || cmd == 0xb4 || cmd == 0xb0 || cmd == 0xb8; }

const char* cmd_name(uint32_t cmd) {
    switch (cmd) {
    case 0x20: return "PUT";
    case 0x21: return "PUTB";
    case 0x22: return "PUTF";
    case 0x24: return "PUTL";
    case 0x25: return "PUTLB";
    case 0x26: return "PUTLF";
    case 0x40: return "GET";
    case 0x41: return "GETB";
    case 0x42: return "GETF";
    case 0x44: return "GETL";
    case 0x45: return "GETLB";
    case 0x46: return "GETLF";
    case 0xd0: return "GETLLAR";
    case 0xb4: return "PUTLLC";
    case 0xb0: return "PUTLLUC";
    case 0xb8: return "PUTQLLUC";
    default: return "CMD";
    }
}

void snapshot_payload(Op& o, const uint8_t* ls) {
    if (!is_put(o.cmd) || is_list(o.cmd)) return;
    uint32_t lsa = o.lsa, size = o.size;
    if (is_atomic(o.cmd)) {
        lsa &= 0x3ff80;
        size = 128;
    }
    if (size > 0x4000) size = 0x4000;
    o.payload.resize(size);
    for (uint32_t i = 0; i < size; i++) o.payload[i] = ls[(lsa + i) & 0x3ffff];
}

std::string op_str(const Op& o) {
    char b[200];
    if (o.kind == kCmd) {
        std::snprintf(b, sizeof b, "%s lsa %05x ea %08x size %x tag %u", cmd_name(o.cmd), o.lsa, o.ea, o.size, o.tag);
        std::string s = b;
        if (o.cmd != 0x20 && o.cmd != 0x40 && o.cmd != 0x22 && !is_atomic(o.cmd)) {
            std::snprintf(b, sizeof b, " (cmd %02x)", o.cmd);
            s += b;
        }
        if (o.eah) {
            std::snprintf(b, sizeof b, " eah %08x", o.eah);
            s += b;
        }
        if (o.has_status) {
            std::snprintf(b, sizeof b, " status %x", o.status);
            s += b;
        }
        return s;
    }
    if (o.kind == kWait) {
        if (o.has_mask)
            std::snprintf(b, sizeof b, "WAIT mask %08x update %u status %08x x%d", o.mask, o.update, o.status, o.count);
        else
            std::snprintf(b, sizeof b, "WAIT mask ? update %u status %08x x%d", o.update, o.status, o.count);
        return b;
    }
    std::snprintf(b, sizeof b, "DEC %08x", o.value);
    return b;
}

class Recorder {
public:
    Recording rec;
    uint32_t cur_mask = 0, cur_update = 0;
    bool has_mask = false;
    bool in_cmd = false;
    const edgecap::Machine* m = nullptr;

    void attach(edgecap::Machine& mach) {
        m = &mach;
        mach.on_write = [this](uint32_t ch, uint32_t v) { on_write(ch, v); };
        mach.on_dma = [this](const edgecap::Dma& d, bool after) { on_dma(d, after); };
        mach.on_read = [this](uint32_t ch, uint32_t v) { on_read(ch, v); };
        mach.on_line = [this](uint32_t lsa) { on_line(lsa); };
    }

    static void detach(edgecap::Machine& mach) {
        mach.on_write = nullptr;
        mach.on_dma = nullptr;
        mach.on_read = nullptr;
        mach.on_line = nullptr;
    }

    void on_write(uint32_t ch, uint32_t v) {
        if (ch == 22) {
            cur_mask = v;
            has_mask = true;
        } else if (ch == 23)
            cur_update = v;
        else if (ch < 16 || ch > 21)
            rec.stray_writes++;
    }

    void on_dma(const edgecap::Dma& d, bool after) {
        if (after) {
            in_cmd = false;
            return;
        }
        Op o;
        o.kind = kCmd;
        o.cmd = d.cmd;
        o.lsa = d.lsa;
        o.eah = d.eah;
        o.ea = d.ea;
        o.size = d.size;
        o.tag = d.tag;
        o.pc = d.pc;
        snapshot_payload(o, m->ls.data());
        rec.ops.push_back(o);
        in_cmd = true;
    }

    void on_read(uint32_t ch, uint32_t v) {
        if (ch == 24) {
            if (!rec.ops.empty() && rec.ops.back().kind == kWait && rec.ops.back().has_mask == has_mask && rec.ops.back().mask == cur_mask) {
                rec.ops.back().count++;
                rec.ops.back().status = v;
                return;
            }
            Op o;
            o.kind = kWait;
            o.mask = cur_mask;
            o.has_mask = has_mask;
            o.update = cur_update;
            o.status = v;
            o.pc = m->c.pc;
            rec.ops.push_back(o);
            return;
        }
        if (ch == 27 && !rec.ops.empty() && rec.ops.back().kind == kCmd && is_atomic(rec.ops.back().cmd) && !rec.ops.back().has_status) {
            rec.ops.back().status = v;
            rec.ops.back().has_status = true;
            return;
        }
        if (ch == 8) {
            Op o;
            o.kind = kDec;
            o.value = v;
            o.pc = m->c.pc;
            rec.ops.push_back(o);
            return;
        }
        rec.reads.push_back(Read{rec.ops.size(), ch, v, m->c.pc});
    }

    void on_line(uint32_t lsa) {
        Line l;
        l.op = in_cmd ? rec.ops.size() - 1 : rec.ops.size();
        l.lsa = lsa;
        std::memcpy(l.data, m->ls.data() + lsa, 128);
        rec.lines.push_back(l);
    }
};

class MockMfc : public edge::glue::Mfc {
public:
    const Recording& rec;
    uint8_t* ls;
    std::vector<Op> ops;
    size_t line_pos = 0;
    size_t dec_pos = 0;
    int dec_missing = 0;

    MockMfc(const Recording& r, uint8_t* l) : rec(r), ls(l) {}

    void apply_lines(size_t upto) {
        while (line_pos < rec.lines.size() && rec.lines[line_pos].op <= upto) {
            std::memcpy(ls + rec.lines[line_pos].lsa, rec.lines[line_pos].data, 128);
            line_pos++;
        }
    }

    const Op* recorded_cmd(size_t k, uint32_t cmd) const {
        if (k < rec.ops.size() && rec.ops[k].kind == kCmd && rec.ops[k].cmd == cmd) return &rec.ops[k];
        return nullptr;
    }

    uint32_t issue(uint32_t cmd, uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag) {
        Op o;
        o.kind = kCmd;
        o.cmd = cmd;
        o.lsa = lsa & 0x3ffff;
        o.ea = ea;
        o.size = size;
        o.tag = tag;
        size_t k = ops.size();
        apply_lines(k);
        snapshot_payload(o, ls);
        uint32_t status = 0;
        if (const Op* r = recorded_cmd(k, cmd); r && r->has_status) {
            o.status = r->status;
            o.has_status = true;
            status = r->status;
        }
        ops.push_back(o);
        return status;
    }

    void get(uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag) override { issue(0x40, lsa, ea, size, tag); }
    void put(uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag, bool fence) override { issue(fence ? 0x22 : 0x20, lsa, ea, size, tag); }
    void getllar(uint32_t lsa, uint32_t ea) override { issue(0xd0, lsa, ea, 128, 0); }
    bool putllc(uint32_t lsa, uint32_t ea) override { return (issue(0xb4, lsa, ea, 128, 0) & 1) == 0; }
    void putlluc(uint32_t lsa, uint32_t ea) override { issue(0xb0, lsa, ea, 128, 0); }

    void wait_tag(uint32_t mask) override {
        Op o;
        o.kind = kWait;
        o.mask = mask;
        o.has_mask = true;
        size_t k = ops.size();
        apply_lines(k);
        if (k < rec.ops.size() && rec.ops[k].kind == kWait) {
            o.status = rec.ops[k].status;
            o.update = rec.ops[k].update;
        }
        ops.push_back(o);
    }

    uint32_t decrementer() override {
        Op o;
        o.kind = kDec;
        size_t k = ops.size();
        apply_lines(k);
        while (dec_pos < rec.ops.size() && rec.ops[dec_pos].kind != kDec) dec_pos++;
        if (dec_pos < rec.ops.size()) {
            o.value = rec.ops[dec_pos].value;
            dec_pos++;
        } else
            dec_missing++;
        ops.push_back(o);
        return o.value;
    }

    void finish() { apply_lines(size_t(-1)); }
};

struct OpCompare {
    bool ok = true;
    int tag_warnings = 0;
    size_t first = 0;
    std::string why;
};

OpCompare compare_ops(const std::vector<Op>& rec, const std::vector<Op>& port) {
    OpCompare r;
    size_t n = rec.size() > port.size() ? rec.size() : port.size();
    for (size_t i = 0; i < n; i++) {
        std::string why;
        if (i >= rec.size())
            why = "extra port op";
        else if (i >= port.size())
            why = "missing port op";
        else {
            const Op& a = rec[i];
            const Op& b = port[i];
            if (a.kind != b.kind)
                why = "kind";
            else if (a.kind == kCmd) {
                if (a.cmd != b.cmd || a.lsa != b.lsa || a.ea != b.ea || a.eah != b.eah || a.size != b.size)
                    why = "cmd/lsa/ea/size";
                else if (!a.payload.empty() && a.payload != b.payload) {
                    size_t off = 0;
                    while (off < a.payload.size() && off < b.payload.size() && a.payload[off] == b.payload[off]) off++;
                    char b2[32];
                    std::snprintf(b2, sizeof b2, "payload at +%zx", off);
                    why = b2;
                }
                if (why.empty() && a.tag != b.tag) r.tag_warnings++;
            } else if (a.kind == kWait) {
                if (a.has_mask && a.mask != b.mask) r.tag_warnings++;
            }
        }
        if (!why.empty() && r.ok) {
            r.ok = false;
            r.first = i;
            r.why = why;
        }
    }
    return r;
}

void print_ranges(const uint8_t* a, const uint8_t* b, uint32_t sp) {
    uint32_t lo = sp > 0x10000 ? sp - 0x10000 : 0;
    for (uint32_t x = 0; x < spu::LS_SIZE;) {
        if ((x >= lo && x < sp) || a[x] == b[x]) {
            x++;
            continue;
        }
        uint32_t y = x;
        while (y < spu::LS_SIZE && (a[y] != b[y] || (y + 1 < spu::LS_SIZE && a[y + 1] != b[y + 1]) || (y + 2 < spu::LS_SIZE && a[y + 2] != b[y + 2]))) y++;
        std::printf(" [%05x,%05x)", x, y);
        x = y;
    }
}

void print_ops(const char* label, const std::vector<Op>& ops) {
    std::printf("    %s ops %zu\n", label, ops.size());
    for (size_t i = 0; i < ops.size(); i++) std::printf("      #%zu %s\n", i, op_str(ops[i]).c_str());
}

bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (char ch : s)
        if (ch < '0' || ch > '9') return false;
    return true;
}

int cmd_gunit(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::printf("usage: gunit <fn_lsa_hex> [max] [caps...]\n");
        return 2;
    }
    uint32_t fn = std::stoul(args[1], nullptr, 16);
    size_t capi = 2;
    int maxhits = 1000000;
    if (args.size() > 2 && all_digits(args[2])) {
        maxhits = std::stoi(args[2]);
        capi = 3;
    }
    bool verbose = std::getenv("EDGE_VERBOSE") != nullptr;
    bool quiet = std::getenv("EDGE_QUIET") != nullptr;
    const GluePort* port = glue_ports().count(fn) ? &glue_ports()[fn] : nullptr;
    if (!port) std::printf("fn %05x: no glue port registered, recording only\n", fn);
    int hits = 0, pass = 0, fail = 0, declined = 0, skipped = 0, tagw = 0, dec_missing = 0;
    uint64_t spu_insns = 0, rec_ops = 0, rec_lines = 0;
    for (const std::string& path : harness::cap_list(args, capi)) {
        edgecap::Capture cap;
        if (!cap.load(path)) {
            std::printf("%s: bad capture\n", path.c_str());
            continue;
        }
        edgecap::Machine m(cap);
        int job = -1;
        bool active = false;
        edgeunit::Hit h;
        size_t pos0 = 0;
        uint64_t n0 = 0;
        Recorder recorder;
        bool stop = false;
        while (!m.done() && !stop) {
            uint32_t pc = m.c.pc;
            if (pc == 0xad98 && harness::edge_loaded(m.c.ls)) job++;
            if (!active && pc == fn && harness::edge_loaded(m.c.ls)) {
                active = true;
                h = edgeunit::Hit{};
                h.cap = path;
                h.job = job;
                h.insn = m.n;
                h.fn = fn;
                h.ret = m.c.gpr[0].u32[3];
                h.sp = m.c.gpr[1].u32[3];
                h.before.ls.assign(m.ls.begin(), m.ls.begin() + spu::LS_SIZE);
                h.before.regs = m.c;
                pos0 = m.pos;
                n0 = m.n;
                recorder = Recorder();
                recorder.attach(m);
            }
            m.step();
            if (!active || !(m.c.pc == h.ret && m.c.gpr[1].u32[3] == h.sp)) continue;
            active = false;
            Recorder::detach(m);
            h.fn_insns = m.n - n0;
            h.pure = m.pos == pos0;
            h.after.ls.assign(m.ls.begin(), m.ls.begin() + spu::LS_SIZE);
            h.after.regs = m.c;
            hits++;
            spu_insns += h.fn_insns;
            if (hits >= maxhits) stop = true;
            bool irq = false;
            for (size_t i = pos0; i < m.pos; i++)
                if (cap.events[i].kind == 5) irq = true;
            const Recording& rec = recorder.rec;
            rec_ops += rec.ops.size();
            rec_lines += rec.lines.size();
            if (irq) {
                skipped++;
                std::printf("  job %d insn %llu: SKIP (interrupt inside the call)\n", h.job, (unsigned long long)h.insn);
                continue;
            }
            if (!port) {
                if (verbose || hits <= 3) {
                    std::printf("  job %d insn %llu sp %05x insns %llu: ops %zu lines %zu reads %zu\n", h.job, (unsigned long long)h.insn, h.sp, (unsigned long long)h.fn_insns, rec.ops.size(), rec.lines.size(), rec.reads.size());
                    print_ops("recorded", rec.ops);
                    for (const Read& rd : rec.reads) std::printf("      rdch %u = %08x at pc %05x (before op #%zu)\n", rd.ch, rd.value, rd.pc, rd.op);
                }
                continue;
            }
            edgeunit::Snapshot ns = h.before;
            ns.ls.resize(spu::LS_SIZE + 16);
            ns.regs.ls = ns.ls.data();
            MockMfc mock(rec, ns.ls.data());
            bool ran = port->run(ns.ls.data(), ns.regs, mock);
            mock.finish();
            if (!ran) {
                declined++;
                std::printf("  job %d insn %llu: DECLINED\n", h.job, (unsigned long long)h.insn);
                continue;
            }
            edgeunit::LsDiff d = edgeunit::diff_ls(ns.ls.data(), h.after.ls.data(), h.sp);
            bool r3ok = !port->has_r3 || ns.regs.gpr[3].u32[3] == h.after.regs.gpr[3].u32[3];
            OpCompare oc = compare_ops(rec.ops, mock.ops);
            tagw += oc.tag_warnings;
            dec_missing += mock.dec_missing;
            bool ok = !d.bytes && r3ok && oc.ok && !mock.dec_missing;
            if (ok) {
                pass++;
                if (!quiet) {
                    std::printf("  job %d insn %llu: PASS ops %zu lines %zu insns %llu", h.job, (unsigned long long)h.insn, rec.ops.size(), rec.lines.size(), (unsigned long long)h.fn_insns);
                    if (oc.tag_warnings) std::printf(" tag-warnings %d", oc.tag_warnings);
                    std::printf("\n");
                }
                if (verbose) print_ops("recorded", rec.ops);
                continue;
            }
            fail++;
            std::printf("  job %d insn %llu sp %05x: FAIL", h.job, (unsigned long long)h.insn, h.sp);
            if (d.bytes) std::printf(" ls_diff %zu [%05x..%05x]", d.bytes, d.first, d.last);
            if (!r3ok) std::printf(" r3 DIFF (%08x vs %08x)", ns.regs.gpr[3].u32[3], h.after.regs.gpr[3].u32[3]);
            if (!oc.ok) {
                std::printf(" ops #%zu %s: rec {%s} port {%s}", oc.first, oc.why.c_str(), oc.first < rec.ops.size() ? op_str(rec.ops[oc.first]).c_str() : "-", oc.first < mock.ops.size() ? op_str(mock.ops[oc.first]).c_str() : "-");
            }
            if (mock.dec_missing) std::printf(" decrementer reads without recorded value %d", mock.dec_missing);
            std::printf("\n");
            if (verbose) {
                if (d.bytes) {
                    std::printf("    ls diff ranges:");
                    print_ranges(ns.ls.data(), h.after.ls.data(), h.sp);
                    std::printf("\n");
                }
                print_ops("recorded", rec.ops);
                print_ops("port", mock.ops);
            }
        }
        Recorder::detach(m);
    }
    std::printf("fn %05x %s: calls %d pass %d fail %d declined %d skipped %d tag-warnings %d dec-missing %d spu-insns %llu (%.0f/call) rec-ops %llu rec-lines %llu\n", fn, port ? port->name : "(no port)", hits, pass, fail, declined, skipped, tagw, dec_missing, (unsigned long long)spu_insns, hits ? double(spu_insns) / hits : 0.0, (unsigned long long)rec_ops, (unsigned long long)rec_lines);
    return fail ? 1 : 0;
}

CommandReg reg_gunit("gunit", cmd_gunit);

}

std::map<uint32_t, GluePort>& glue_ports() {
    static std::map<uint32_t, GluePort> m;
    return m;
}
