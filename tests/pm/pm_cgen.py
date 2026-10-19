import os
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tests", "renjob"))
import rj_cgen as RJ

PM_EA = 0x7A7B00
PM_LS = 0xA00
PM_SIZE = 0x2FF0
CODE = ((0xA00, 0xAF0), (0x16D0, 0x39D8))
DATA = (0xAF0, 0x16D0)
API_TABLE = (0x1370, 9)
ENTRIES = (0xA00, 0xA2C, 0x3368)
LOOP_BUDGET = True
TRAMPOLINE = 0x3FEC0

RJ.configure(IMG_EA=PM_EA, IMG_LS=PM_LS, IMG_SIZE=PM_SIZE, CODE_END=0x39D8)
D = RJ.decode
word = RJ.word

BRANCH_COND = {"brz": "pref(r%d) == 0", "brnz": "pref(r%d) != 0", "brhz": "hw6(r%d) == 0", "brhnz": "hw6(r%d) != 0"}
IBRANCH_COND = {"biz": "pref(r%d) == 0", "binz": "pref(r%d) != 0", "bihz": "hw6(r%d) == 0", "bihnz": "hw6(r%d) != 0"}
SKIP = {"hbrr", "hbr", "hbra", "nop", "lnop", "sync", "dsync"}
EXIT_BEFORE = {"stop", "stopd", "bisled", "invalid", "heqi", "hgti", "hlgti", "heq", "hgt", "hlgt", "mtspr", "fscrwr", "fscrrd"}


def in_code(pc):
    return any(a <= pc < b for a, b in CODE)


def s16(d):
    return d["imm"]


def rel(pc, d):
    return (pc + d["imm"] * 4) & 0x3FFFC


def absolute(d):
    return (d["imm"] * 4) & 0x3FFFC


def address_taken_static():
    out = set(ENTRIES)
    base, n = API_TABLE
    for i in range(n):
        out.add(word(base + 4 * i) & 0x3FFFC)
    for a in range(DATA[0], DATA[1], 4):
        v = word(a)
        if v & 3 == 0 and in_code(v):
            out.add(v)
    return out


def discover():
    roots = set(address_taken_static())
    insns = {}
    labels = set(roots)
    taken = set(roots)
    work = sorted(roots)
    while work:
        pc = work.pop()
        while pc not in insns:
            if not in_code(pc):
                break
            d = D(pc)
            insns[pc] = d
            op = d["op"]
            nxt = pc + 4
            if op in ("il", "ila") and d["imm"] & 3 == 0 and in_code(d["imm"]):
                v = d["imm"]
                taken.add(v)
                if v not in labels:
                    labels.add(v)
                    work.append(v)
            if op in ("brsl", "brasl", "bisl"):
                taken.add(nxt)
                if nxt not in labels:
                    labels.add(nxt)
                    work.append(nxt)
            if op == "br":
                t = rel(pc, d)
                if in_code(t):
                    labels.add(t)
                    work.append(t)
                break
            if op == "bra":
                t = absolute(d)
                if in_code(t):
                    labels.add(t)
                    work.append(t)
                break
            if op in BRANCH_COND:
                t = rel(pc, d)
                if in_code(t):
                    labels.add(t)
                    work.append(t)
                labels.add(nxt)
            if op == "brsl":
                t = rel(pc, d)
                if in_code(t):
                    labels.add(t)
                    work.append(t)
                break
            if op == "brasl":
                t = absolute(d)
                if in_code(t):
                    labels.add(t)
                    work.append(t)
                break
            if op in ("bi", "bisl", "iret") or op in EXIT_BEFORE:
                break
            if op in IBRANCH_COND:
                labels.add(nxt)
            if op in ("rdch", "wrch", "rchcnt"):
                labels.add(nxt)
                if op == "wrch" and d["ra"] in (1, 2):
                    taken.add(nxt)
            pc = nxt
    return insns, labels, taken


def leaders_of(insns, labels):
    lead = set(l for l in labels if l in insns)
    for pc, d in insns.items():
        op = d["op"]
        if op in ("br", "bra", "brsl", "brasl", "bi", "bisl", "iret") or op in BRANCH_COND or op in IBRANCH_COND or op in ("rdch", "wrch", "rchcnt"):
            if pc + 4 in insns:
                lead.add(pc + 4)
        if pc - 4 not in insns:
            lead.add(pc)
    return lead


def loop_heads(insns):
    heads = set()
    for pc, d in insns.items():
        op = d["op"]
        if op in ("br", "brsl") or op in BRANCH_COND:
            t = rel(pc, d)
            if t <= pc and t in insns:
                heads.add(t)
        if op in ("bra", "brasl"):
            t = absolute(d)
            if t <= pc and t in insns:
                heads.add(t)
    return heads


def interrupt_bits(d, target_expr):
    rb = d["rb"]
    if rb & 0x10:
        return "c.interrupts_enabled = true; c.irq_check = true; X(%s, 0);" % target_expr, True
    if rb & 0x20:
        return "c.interrupts_enabled = false;", False
    return "", False


def emit(out_cpp, out_h):
    insns, labels, taken = discover()
    lead = leaders_of(insns, labels)
    heads = loop_heads(insns) if LOOP_BUDGET else set()
    hooks = sorted((taken | heads) & set(insns))
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
    o = []
    o.append('#include "pm_gen.h"')
    o.append("")
    o.append("namespace pm::gen {")
    o.append("")
    o.append("#define X(target, rem) do { c.pc = (target); c.executed = uint32_t(cnt - (rem)); return true; } while (0)")
    o.append("#define XC(rem) do { c.executed = uint32_t(cnt - (rem)); return true; } while (0)")
    o.append("")
    o.append("bool run(spu::Context& c, spu::Host& h, uint64_t budget) {")
    o.append("    V* __restrict R = reinterpret_cast<V*>(c.gpr);")
    o.append("    uint8_t* __restrict ls = c.ls;")
    o.append("    uint64_t cnt = 0;")
    o.append("    uint32_t tgt = 0;")
    o.append("    uint64_t lim = c.interrupts_enabled && budget > kIrqSlice ? kIrqSlice : budget;")
    regs = sorted({r for d in insns.values() for k in ("rt", "ra", "rb", "rc") if k in d for r in [d[k]]})
    o.append("    " + " ".join("V& r%d = R[%d];" % (r, r) for r in regs))
    o.append("    switch (c.pc) {")
    for hk in hooks:
        o.append("    case 0x%05x: goto L_%05x;" % (hk, hk))
    o.append("    default: return false;")
    o.append("    }")
    prev_fall = None
    for pc in pcs:
        d = insns[pc]
        op = d["op"]
        b, i, n = pos[pc]
        rem = n - i
        after = n - i - 1
        if i == 0:
            if prev_fall is not None and prev_fall != pc:
                pass
            o.append("L_%05x:" % pc)
            if pc in heads:
                o.append("    if (__builtin_expect(cnt >= lim, 0)) X(0x%05x, 0);" % pc)
            o.append("    cnt += %d;" % n)
        rt, ra, rb, rc, imm = d.get("rt"), d.get("ra"), d.get("rb"), d.get("rc"), d.get("imm")
        s = None
        nxt = pc + 4
        fall_ok = True
        if op in SKIP:
            s = ""
        elif op in EXIT_BEFORE:
            s = "X(0x%05x, %d);" % (pc, rem)
            fall_ok = False
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
            s = "r%d = lq(ls, 0x%x);" % (rt, (pc + imm * 4) & 0x3FFFF)
        elif op == "lqa":
            s = "r%d = lq(ls, 0x%x);" % (rt, (imm * 4) & 0x3FFFF)
        elif op == "stqd":
            s = "stm(c, ls, pref(r%d) + %d, r%d);" % (ra, imm * 16, rt)
        elif op == "stqx":
            s = "stm(c, ls, pref(r%d) + pref(r%d), r%d);" % (ra, rb, rt)
        elif op == "stqr":
            s = "stm(c, ls, 0x%x, r%d);" % ((pc + imm * 4) & 0x3FFFF, rt)
        elif op == "stqa":
            s = "stm(c, ls, 0x%x, r%d);" % ((imm * 4) & 0x3FFFF, rt)
        elif op == "mfspr":
            s = "r%d = _mm_setzero_si128();" % rt
        elif op == "rdch":
            s = "c.pc = 0x%05x; r%d = pref_only(h.read_channel(%d)); if (c.halted || c.pc != 0x%05x) XC(%d);" % (nxt, rt, ra, nxt, after)
        elif op == "rchcnt":
            s = "c.pc = 0x%05x; r%d = pref_only(h.channel_count(%d)); if (c.halted || c.pc != 0x%05x) XC(%d);" % (nxt, rt, ra, nxt, after)
        elif op == "wrch":
            s = "c.pc = 0x%05x; h.write_channel(%d, pref(r%d)); if (c.halted || c.pc != 0x%05x) XC(%d);" % (nxt, ra, rt, nxt, after)
        elif op in ("br", "bra"):
            t = rel(pc, d) if op == "br" else absolute(d)
            s = ("goto L_%05x;" % t) if t in insns else ("X(0x%05x, 0);" % t)
            fall_ok = False
        elif op == "brasl" and absolute(d) == TRAMPOLINE:
            s = "r%d = pref_only(0x%x); { uint32_t tp = run_trampoline(c, ls, R, cnt, %d); if (!tp) { tgt = pref(r%d) & 0x3fffc; goto DISPATCH; } X(tp, 0); }" % (rt, nxt, rt, rt)
            fall_ok = False
        elif op in ("brsl", "brasl"):
            t = rel(pc, d) if op == "brsl" else absolute(d)
            s = "r%d = pref_only(0x%x); " % (rt, nxt) + (("goto L_%05x;" % t) if t in insns else ("X(0x%05x, 0);" % t))
            fall_ok = False
        elif op in BRANCH_COND:
            t = rel(pc, d)
            cond = BRANCH_COND[op] % rt
            s = "if (%s) %s" % (cond, ("goto L_%05x;" % t) if t in insns else ("X(0x%05x, 0);" % t))
        elif op in ("bi", "bisl"):
            bits, exits = interrupt_bits(d, "tgt")
            s = "tgt = pref(r%d) & 0x3fffc; " % ra
            if op == "bisl":
                s += "r%d = pref_only(0x%x); " % (rt, nxt)
            s += bits + ("" if exits else " goto DISPATCH;")
            fall_ok = False
        elif op in IBRANCH_COND:
            bits, exits = interrupt_bits(d, "tgt")
            cond = IBRANCH_COND[op] % rt
            s = "if (%s) { tgt = pref(r%d) & 0x3fffc; %s%s }" % (cond, ra, bits, "" if exits else " goto DISPATCH;")
        elif op == "iret":
            bits, exits = interrupt_bits(d, "tgt")
            s = "tgt = c.srr0 & 0x3fffc; " + bits + ("" if exits else " X(tgt, 0);")
            fall_ok = False
        else:
            raise Exception("unhandled %s at %05x" % (op, pc))
        if s:
            o.append("    " + s)
        if fall_ok and i == n - 1:
            if nxt in insns:
                o.append("    goto L_%05x;" % nxt)
            else:
                o.append("    X(0x%05x, 0);" % nxt)
    o.append("DISPATCH:")
    o.append("    if (__builtin_expect(cnt >= lim, 0)) X(tgt, 0);")
    o.append("    switch (tgt) {")
    for l in sorted(set(insns) & lead):
        o.append("    case 0x%05x: goto L_%05x;" % (l, l))
    o.append("    default: X(tgt, 0);")
    o.append("    }")
    o.append("}")
    o.append("")
    o.append("const uint32_t kHooks[] = {%s};" % ", ".join("0x%05xu" % h for h in hooks))
    o.append("const size_t kHookCount = sizeof(kHooks) / sizeof(kHooks[0]);")
    o.append("const uint32_t kHeader[] = {%s};" % ", ".join("0x%08xu" % word(a) for a in range(PM_LS, PM_LS + 0x40, 4)))
    o.append("const uint32_t kCode[] = {%s};" % ", ".join("0x%08xu" % word(a) for a in range(PM_LS, CODE[-1][1], 4)))
    o.append("")
    o.append("#undef X")
    o.append("#undef XC")
    o.append("")
    o.append("}")
    open(out_cpp, "w", newline="\n").write("\n".join(o) + "\n")
    hh = ["#pragma once", "", '#include "pm_ops.h"', "", "namespace pm::gen {", "",
          "constexpr uint32_t kImageLs = 0x%x;" % PM_LS, "constexpr uint32_t kHeaderWords = 16;",
          "constexpr uint32_t kCodeEnd = 0x%x;" % CODE[-1][1], "constexpr uint64_t kIrqSlice = 256;",
          "bool run(spu::Context& c, spu::Host& h, uint64_t budget);", "extern const uint32_t kHooks[];", "extern const size_t kHookCount;", "extern const uint32_t kHeader[];", "extern const uint32_t kCode[];", "", "}"]
    open(out_h, "w", newline="\n").write("\n".join(hh) + "\n")
    print("%d insns, %d blocks, %d hooks, %d loop heads, %d labels" % (len(insns), len(blocks), len(hooks), len(heads), len(labels)), file=sys.stderr)


if __name__ == "__main__":
    emit(sys.argv[1], sys.argv[2])
