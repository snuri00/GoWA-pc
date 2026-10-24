import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tests", "renjob"))
sys.path.insert(0, HERE)
import rj_cgen as RJ
import callbacks

ELF = os.environ.get("GOWA_ELF") or os.path.join(ROOT, "bin", "01.12", "GOWA.patched.elf")
NS = "edge::ovl"
OPS_H = "../pm/pm_ops.h"

BRANCH_COND = {"brz": "pref(r%d) == 0", "brnz": "pref(r%d) != 0", "brhz": "hw6(r%d) == 0", "brhnz": "hw6(r%d) != 0"}
IBRANCH_COND = {"biz": "pref(r%d) == 0", "binz": "pref(r%d) != 0", "bihz": "hw6(r%d) == 0", "bihnz": "hw6(r%d) != 0"}
SKIP = {"hbrr", "hbr", "hbra", "nop", "lnop", "sync", "dsync"}
EXIT_BEFORE = {"stop", "stopd", "bisled", "invalid", "mtspr", "fscrwr", "fscrrd", "iret"}


class Image:
    def __init__(self, name, ea, code):
        self.name = name
        self.ea = ea
        self.code = code
        self.size = len(code)

    def word(self, off):
        return struct.unpack_from(">I", self.code, off)[0]

    def decode(self, off):
        RJ.img = self.code
        RJ.IMG_LS = 0
        return RJ.decode(off)


def rel(off, d):
    return off + d["imm"] * 4


def discover(im):
    insns = {}
    labels = {0}
    work = [0]
    while work:
        pc = work.pop()
        while pc not in insns:
            if pc < 0 or pc + 4 > im.size:
                break
            d = im.decode(pc)
            insns[pc] = d
            op = d["op"]
            nxt = pc + 4
            if op in ("brsl", "brasl", "bisl"):
                if nxt not in labels:
                    labels.add(nxt)
                    work.append(nxt)
            if op in ("br", "brsl"):
                t = rel(pc, d)
                if 0 <= t < im.size:
                    labels.add(t)
                    work.append(t)
                break
            if op in ("bra", "brasl"):
                break
            if op in BRANCH_COND:
                t = rel(pc, d)
                if 0 <= t < im.size:
                    labels.add(t)
                    work.append(t)
                labels.add(nxt)
            if op in ("bi", "bisl") or op in EXIT_BEFORE:
                break
            if op in IBRANCH_COND:
                labels.add(nxt)
            if op in ("rdch", "wrch", "rchcnt"):
                labels.add(nxt)
            pc = nxt
    return insns, labels


def emit_image(im, fname, o, stats):
    insns, labels = discover(im)
    lead = set(l for l in labels if l in insns)
    heads = set()
    for pc, d in insns.items():
        op = d["op"]
        if op in ("br", "bra", "brsl", "brasl", "bi", "bisl", "iret") or op in BRANCH_COND or op in IBRANCH_COND or op in ("rdch", "wrch", "rchcnt"):
            if pc + 4 in insns:
                lead.add(pc + 4)
        if pc - 4 not in insns:
            lead.add(pc)
        if op in ("br", "brsl") or op in BRANCH_COND:
            t = rel(pc, d)
            if t <= pc and t in insns:
                heads.add(t)
    pcs = sorted(insns)
    blocks = {}
    cur = None
    for pc in pcs:
        if pc in lead or cur is None or pc - 4 not in insns:
            cur = pc
            blocks[cur] = []
        blocks[cur].append(pc)
    pos = {}
    for b, members in blocks.items():
        for i, pc in enumerate(members):
            pos[pc] = (b, i, len(members))
    exits = 0
    o.append("bool %s(spu::Context& c, spu::Host& h, uint32_t B, uint64_t budget) {" % fname)
    o.append("    V* __restrict R = reinterpret_cast<V*>(c.gpr);")
    o.append("    uint8_t* __restrict ls = c.ls;")
    o.append("    uint64_t cnt = 0;")
    o.append("    uint32_t tgt = 0;")
    o.append("    (void)h;")
    o.append("    (void)tgt;")
    o.append("    uint64_t lim = c.interrupts_enabled && budget > kIrqSlice ? kIrqSlice : budget;")
    o.append("    (void)lim;")
    regs = sorted({r for d in insns.values() for k in ("rt", "ra", "rb", "rc") if k in d for r in [d[k]]})
    o.append("    " + " ".join("V& r%d = R[%d];" % (r, r) for r in regs))
    o.append("    goto L_%04x;" % 0)
    for pc in pcs:
        d = insns[pc]
        op = d["op"]
        b, i, n = pos[pc]
        rem = n - i
        after = n - i - 1
        if i == 0:
            o.append("L_%04x:" % pc)
            if pc in heads:
                o.append("    if (__builtin_expect(cnt >= lim, 0)) X(B + 0x%x, 0);" % pc)
            o.append("    cnt += %d;" % n)
        rt, ra, rb, rc, imm = d.get("rt"), d.get("ra"), d.get("rb"), d.get("rc"), d.get("imm")
        s = None
        nxt = pc + 4
        fall_ok = True
        P = lambda off: "((B + 0x%x) & 0x3ffff)" % off
        J = lambda t: ("goto L_%04x;" % t) if t in insns else ("X((B + %d) & 0x3fffc, 0);" % t)
        if op in SKIP:
            s = ""
        elif op in EXIT_BEFORE:
            s = "X(B + 0x%x, %d);" % (pc, rem)
            fall_ok = False
            exits += 1
        elif op in RJ.HALTS:
            s = "if (%s) X(B + 0x%x, %d);" % (re.sub("R.([0-9]+).", lambda m: "r" + m.group(1), RJ.HALTS[op] % d), pc, rem)
        elif op in RJ.RR3:
            s = "r%d = %s(r%d, r%d);" % (rt, RJ.cn(op), ra, rb)
        elif op in RJ.RR2:
            s = "r%d = %s(r%d);" % (rt, RJ.cn(op), ra)
        elif op in RJ.RI:
            s = "r%d = %s(r%d, %d);" % (rt, RJ.cn(op), ra, imm)
        elif op in ("selb", "shufb", "fma", "fms", "fnms", "mpya"):
            s = "r%d = %s(r%d, r%d, r%d);" % (rt, op, ra, rb, rc)
        elif op in ("addx", "sfx", "cgx", "bgx"):
            s = "r%d = %s(r%d, r%d, r%d);" % (rt, op, ra, rb, rt)
        elif op in ("cflts", "cfltu", "cuflt", "csflt"):
            s = "r%d = %s(r%d, %d);" % (rt, op, ra, imm)
        elif op == "il":
            s = "r%d = il(%d);" % (rt, imm)
        elif op == "ilh":
            s = "r%d = ilh(0x%x);" % (rt, d["u16"])
        elif op == "ilhu":
            s = "r%d = ilhu(0x%x);" % (rt, d["u16"])
        elif op == "iohl":
            s = "r%d = iohl(r%d, 0x%x);" % (rt, rt, d["u16"])
        elif op == "ila":
            s = "r%d = ila(0x%x);" % (rt, imm)
        elif op == "fsmbi":
            s = "r%d = fsmbi(0x%x);" % (rt, d["u16"])
        elif op == "lqd":
            s = "r%d = lq(ls, pref(r%d) + %d);" % (rt, ra, imm * 16)
        elif op == "lqx":
            s = "r%d = lq(ls, pref(r%d) + pref(r%d));" % (rt, ra, rb)
        elif op == "lqr":
            s = "r%d = lq(ls, %s);" % (rt, P(pc + imm * 4))
        elif op == "lqa":
            s = "r%d = lq(ls, 0x%x);" % (rt, (imm * 4) & 0x3FFFF)
        elif op == "stqd":
            s = "stm(c, ls, pref(r%d) + %d, r%d);" % (ra, imm * 16, rt)
        elif op == "stqx":
            s = "stm(c, ls, pref(r%d) + pref(r%d), r%d);" % (ra, rb, rt)
        elif op == "stqr":
            s = "stm(c, ls, %s, r%d);" % (P(pc + imm * 4), rt)
        elif op == "stqa":
            s = "stm(c, ls, 0x%x, r%d);" % ((imm * 4) & 0x3FFFF, rt)
        elif op == "mfspr":
            s = "r%d = _mm_setzero_si128();" % rt
        elif op == "rdch":
            s = "c.pc = %s; r%d = pref_only(h.read_channel(%d)); if (c.halted || c.pc != %s) XC(%d);" % (P(nxt), rt, ra, P(nxt), after)
        elif op == "rchcnt":
            s = "c.pc = %s; r%d = pref_only(h.channel_count(%d)); if (c.halted || c.pc != %s) XC(%d);" % (P(nxt), rt, ra, P(nxt), after)
        elif op == "wrch":
            s = "c.pc = %s; h.write_channel(%d, pref(r%d)); if (c.halted || c.pc != %s) XC(%d);" % (P(nxt), ra, rt, P(nxt), after)
        elif op == "br":
            s = J(rel(pc, d))
            fall_ok = False
        elif op == "bra":
            s = "X(0x%x, 0);" % ((imm * 4) & 0x3FFFC)
            fall_ok = False
        elif op == "brsl":
            s = "r%d = pref_only(%s); %s" % (rt, P(nxt), J(rel(pc, d)))
            fall_ok = False
        elif op == "brasl":
            s = "r%d = pref_only(%s); X(0x%x, 0);" % (rt, P(nxt), (imm * 4) & 0x3FFFC)
            fall_ok = False
        elif op in BRANCH_COND:
            s = "if (%s) %s" % (BRANCH_COND[op] % rt, J(rel(pc, d)))
        elif op in ("bi", "bisl"):
            s = "tgt = pref(r%d) & 0x3fffc; " % ra
            if op == "bisl":
                s += "r%d = pref_only(%s); " % (rt, P(nxt))
            if d["rb"] & 0x30:
                s = "X(B + 0x%x, %d);" % (pc, rem)
                exits += 1
            else:
                s += "goto DISPATCH;"
            fall_ok = False
        elif op in IBRANCH_COND:
            if d["rb"] & 0x30:
                s = "X(B + 0x%x, %d);" % (pc, rem)
                exits += 1
            else:
                s = "if (%s) { tgt = pref(r%d) & 0x3fffc; goto DISPATCH; }" % (IBRANCH_COND[op] % rt, ra)
        else:
            s = "X(B + 0x%x, %d);" % (pc, rem)
            fall_ok = False
            exits += 1
            stats.setdefault("ops", {}).setdefault(op, 0)
            stats["ops"][op] += 1
        if s:
            o.append("    " + s)
        if fall_ok and i == n - 1:
            if nxt in insns:
                o.append("    goto L_%04x;" % nxt)
            else:
                o.append("    X(%s, 0);" % P(nxt))
    targets = sorted(set(insns) & lead)
    o.append("DISPATCH:")
    o.append("    if (__builtin_expect(cnt >= lim, 0)) X(tgt, 0);")
    o.append("    switch (tgt - B) {")
    for l in targets:
        o.append("    case 0x%x: goto L_%04x;" % (l, l))
    o.append("    default: X(tgt, 0);")
    o.append("    }")
    o.append("}")
    o.append("")
    stats["insns"] = stats.get("insns", 0) + len(insns)
    stats["exits"] = stats.get("exits", 0) + exits
    return len(insns), exits


def main(out_cpp, out_h):
    elf = open(ELF, "rb").read()
    table = callbacks.table(elf)
    seen = {}
    imgs = []
    for name, ea, sz in sorted(table, key=lambda t: t[1]):
        if ea in seen:
            continue
        seen[ea] = name
        imgs.append(Image(name, ea, elf[ea - 0x10000: ea - 0x10000 + sz]))
    o = ['#include "%s"' % os.path.basename(out_h), "", '#include "%s"' % OPS_H, "", "namespace %s {" % NS, "", "using namespace pm::gen;", "",
         "#define X(target, rem) do { c.pc = (target); c.executed = uint32_t(cnt - (rem)); return true; } while (0)",
         "#define XC(rem) do { c.executed = uint32_t(cnt - (rem)); return true; } while (0)", "", "namespace {", ""]
    stats = {}
    rows = []
    for k, im in enumerate(imgs):
        ins, _ = discover(im)
        if any(d["op"] in ("rdch", "wrch", "rchcnt", "stop", "stopd") for d in ins.values()):
            print("%-26s ea %06x size %4x: channel ops, not generated" % (im.name, im.ea, im.size), file=sys.stderr)
            continue
        fn = "ovl_%06x" % im.ea
        n, ex = emit_image(im, fn, o, stats)
        rows.append((im, fn))
        print("%-26s ea %06x size %4x: %4d insns, %d exits" % (im.name, im.ea, im.size, n, ex), file=sys.stderr)
    for im, fn in rows:
        o.append("const uint8_t k_%06x[] = {%s};" % (im.ea, ",".join(str(b) for b in im.code)))
    o.append("")
    o.append("}")
    o.append("")
    o.append("const Entry kEntries[] = {")
    for im, fn in rows:
        head = struct.unpack_from("<Q", im.code, 0)[0]
        o.append('    {0x%06xu, 0x%xu, 0x%016xull, k_%06x, %s, "%s"},' % (im.ea, im.size, head, im.ea, fn, im.name))
    o.append("};")
    o.append("const size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);")
    o.append("")
    o.append("#undef X")
    o.append("#undef XC")
    o.append("")
    o.append("}")
    open(out_cpp, "w", newline="\n").write("\n".join(o) + "\n")
    hh = ["#pragma once", "", '#include "spu/context.h"', "", "#include <cstddef>", "#include <cstdint>", "", "namespace %s {" % NS, "",
          "constexpr uint64_t kIrqSlice = 65536;", "",
          "struct Entry {", "    uint32_t ea;", "    uint32_t size;", "    uint64_t head;", "    const uint8_t* code;",
          "    bool (*run)(spu::Context& c, spu::Host& h, uint32_t base, uint64_t budget);", "    const char* name;", "};", "",
          "extern const Entry kEntries[];", "extern const size_t kEntryCount;", "", "}"]
    open(out_h, "w", newline="\n").write("\n".join(hh) + "\n")
    print("%d images, %d insns, %d exits, unsupported ops %s" % (len(rows), stats.get("insns", 0), stats.get("exits", 0), stats.get("ops", {})), file=sys.stderr)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
