import os
import re
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import spu_dis as D

ELF = os.path.join(ROOT, "bin", "01.12", "GOWA.patched.elf")
IMG_EA = 0x705680
IMG_LS = 0x4000
IMG_SIZE = 0x13580

data = open(ELF, "rb").read()
img = data[IMG_EA - 0x10000:IMG_EA - 0x10000 + IMG_SIZE]


def word(pc):
    return struct.unpack_from(">I", img, pc - IMG_LS)[0]


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def decode(pc):
    w = word(pc)
    rt, ra, rb = w & 0x7F, (w >> 7) & 0x7F, (w >> 14) & 0x7F
    if (w >> 28) in D.RRR:
        return dict(op=D.RRR[w >> 28], rt=(w >> 21) & 0x7F, ra=ra, rb=rb, rc=w & 0x7F, w=w)
    if (w >> 25) in D.RI18:
        return dict(op=D.RI18[w >> 25], rt=rt, imm=(w >> 7) & 0x3FFFF, w=w)
    if (w >> 24) in D.RI10:
        return dict(op=D.RI10[w >> 24], rt=rt, ra=ra, imm=sext((w >> 14) & 0x3FF, 10), w=w)
    if (w >> 23) in D.RI16:
        return dict(op=D.RI16[w >> 23], rt=rt, imm=sext((w >> 7) & 0xFFFF, 16), u16=(w >> 7) & 0xFFFF, w=w)
    if (w >> 22) in D.RI8:
        return dict(op=D.RI8[w >> 22], rt=rt, ra=ra, imm=(w >> 14) & 0xFF, w=w)
    if (w >> 21) in D.RR:
        op = D.RR[w >> 21]
        d = dict(op=op, rt=rt, ra=ra, rb=rb, w=w)
        if op in D.RI7_OPS:
            d["imm"] = sext((w >> 14) & 0x7F, 7)
        return d
    return dict(op="invalid", w=w)


SKIP = {"hbrr", "hbr", "hbra", "nop", "lnop", "sync", "dsync"}
FAILOPS = {"rdch", "wrch", "rchcnt", "stop", "stopd", "invalid", "iret", "bisled", "mfspr", "mtspr", "fscrrd", "fscrwr"}
HALTS = {"heqi": "int32_t(pref(R[%(ra)d])) == %(imm)d", "hgti": "int32_t(pref(R[%(ra)d])) > %(imm)d", "hlgti": "pref(R[%(ra)d]) > uint32_t(%(imm)d)",
         "heq": "pref(R[%(ra)d]) == pref(R[%(rb)d])", "hgt": "int32_t(pref(R[%(ra)d])) > int32_t(pref(R[%(rb)d]))", "hlgt": "pref(R[%(ra)d]) > pref(R[%(rb)d])"}
RR3 = {"a", "ah", "sf", "sfh", "and", "andc", "or", "xor", "nor", "nand", "orc", "eqv", "cg", "bg", "mpy", "mpyu", "mpyh", "mpyhhu", "mpyhh", "mpys",
       "shl", "shlh", "rot", "rotm", "rotma", "roth", "rothm", "rotmah", "ceq", "ceqh", "ceqb", "cgt", "cgth", "cgtb", "clgt", "clgth", "clgtb",
       "shlqby", "rotqby", "rotqmby", "shlqbi", "rotqbi", "rotqmbi", "shlqbybi", "rotqbybi", "rotqmbybi", "fa", "fs", "fm", "fcgt", "fceq", "fcmgt", "fcmeq", "fi",
       "cbx", "chx", "cwx", "cdx", "sumb", "avgb", "absdb"}
RR2 = {"cntb", "xsbh", "xshw", "xswd", "fsmb", "fsmh", "fsm", "gb", "gbh", "gbb", "frest", "frsqest", "orx", "clz"}
RI = {"ai", "ahi", "sfi", "sfhi", "andi", "andhi", "andbi", "ori", "orhi", "orbi", "xori", "xorhi", "xorbi", "mpyi", "mpyui",
      "ceqi", "ceqhi", "ceqbi", "cgti", "cgthi", "cgtbi", "clgti", "clgthi", "clgtbi",
      "shli", "shlhi", "roti", "rothi", "rotmi", "rotmai", "rothmi", "rotmahi", "shlqbyi", "rotqbyi", "rotqmbyi", "shlqbii", "rotqbii", "rotqmbii",
      "cbd", "chd", "cwd", "cdd"}
CPPNAME = {"and": "and_", "or": "or_", "xor": "xor_"}


def cn(op):
    return CPPNAME.get(op, op)


def jump_table(pc):
    base = pc + 4
    out = []
    while True:
        v = word(base + 4 * len(out))
        if v >= 0x10000 or v & 3 or v == 0:
            break
        out.append(base + v)
    return out


class Func:
    def __init__(self, entry):
        self.entry = entry
        self.insns = {}
        self.labels = set()
        self.calls = set()
        self.tails = set()
        self.exits = set()
        self.multi = None


def discover(entry, known_entries, stops=frozenset(), entries=None):
    f = Func(entry)
    f.exits = set()
    work = list(entries) if entries else [entry]
    eset = set(work)
    f.multi = sorted(entries) if entries and len(entries) > 1 else None
    if f.multi:
        f.labels |= eset
    while work:
        pc = work.pop()
        while pc not in f.insns:
            if pc in stops and pc not in eset:
                f.exits.add(pc)
                break
            d = decode(pc)
            f.insns[pc] = d
            op = d["op"]
            nxt = pc + 4
            if op == "br":
                t = (pc + d["imm"] * 4) & 0x3FFFC
                if t in known_entries and t != entry:
                    f.tails.add(t)
                else:
                    f.labels.add(t)
                    work.append(t)
                break
            if op in ("brz", "brnz", "brhz", "brhnz"):
                t = (pc + d["imm"] * 4) & 0x3FFFC
                f.labels.add(t)
                work.append(t)
            if op == "brsl":
                t = (pc + d["imm"] * 4) & 0x3FFFC
                if t != nxt:
                    f.calls.add(t)
            if op == "bi" and d["ra"] != 0 and d["rb"] & 0x30 == 0 and jump_table(pc):
                f.switches = getattr(f, "switches", {})
                f.switches[pc] = jump_table(pc)
                for t in f.switches[pc]:
                    f.labels.add(t)
                    work.append(t)
                break
            if op in ("bi", "bra", "brasl") or op in FAILOPS - {"rdch", "wrch", "rchcnt", "mfspr", "mtspr", "fscrrd", "fscrwr"}:
                break
            pc = nxt
    return f


def reads_of(d):
    op = d["op"]
    rs = set()
    if op in ("stqd", "stqx", "stqr", "stqa", "wrch", "brz", "brnz", "brhz", "brhnz", "biz", "binz", "bihz", "bihnz") and "rt" in d:
        rs.add(d["rt"])
    if op in ("selb", "shufb", "fma", "fms", "fnms", "mpya"):
        rs |= {d["ra"], d["rb"], d["rc"]}
        return rs
    if op in ("addx", "sfx", "cgx", "bgx", "iohl"):
        rs.add(d["rt"])
    if op in ("il", "ilh", "ilhu", "ila", "fsmbi", "lqr", "lqa", "stqr", "stqa", "br", "brsl", "bra", "brasl", "stop", "stopd", "lnop", "nop", "hbrr", "hbra", "rdch", "rchcnt", "iohl", "invalid"):
        return rs
    if op in ("bi", "bisl", "biz", "binz", "bihz", "bihnz", "hbr"):
        rs.add(d["ra"])
        return rs
    if op in RR2:
        rs.add(d["ra"])
        return rs
    if op == "wrch":
        return {d["rt"]}
    for k in ("ra", "rb"):
        if k in d:
            rs.add(d[k])
    if op in D.RI7_OPS or (d["w"] >> 24) in D.RI10 or (d["w"] >> 22) in D.RI8:
        rs.discard(d.get("rb"))
        rs.add(d["ra"])
    return rs


def writes_r0(d):
    op = d["op"]
    if d.get("rt") != 0:
        return False
    return op not in SKIP and op not in NODEF and op not in ("rchcnt",) or op == "rchcnt"


def r0_dead(x):
    seen = set()
    work = [x]
    while work:
        pc = work.pop()
        while pc not in seen:
            seen.add(pc)
            d = decode(pc)
            op = d["op"]
            if op in SKIP:
                pc += 4
                continue
            if 0 in reads_of(d):
                return False
            if writes_r0(d):
                break
            if op == "br":
                pc = (pc + d["imm"] * 4) & 0x3FFFC
                continue
            if op in ("brz", "brnz", "brhz", "brhnz"):
                work.append((pc + d["imm"] * 4) & 0x3FFFC)
            if op == "brsl":
                if d["rt"] == 0:
                    break
            if op == "bi":
                prev, prev2 = decode(pc - 4), decode(pc - 8)
                if prev["op"] == "ai" and prev["ra"] == d["ra"] and prev["imm"] == 8 and prev2["op"] == "brsl" and prev2["rt"] == d["ra"] and (pc - 8 + prev2["imm"] * 4) == pc - 4:
                    pc += 4
                    continue
                if d["ra"] != 0 and d["rb"] & 0x30 == 0 and jump_table(pc):
                    work.extend(jump_table(pc))
                    break
                return False
            if op in ("bisl", "bra", "brasl", "stop", "stopd", "invalid", "iret", "bisled"):
                return False
            pc += 4
    return True


def regs_of(d):
    rs = set()
    for k in ("rt", "ra", "rb", "rc"):
        if k in d:
            rs.add(d[k])
    return rs


NODEF = {"stqd", "stqx", "stqr", "stqa", "br", "brz", "brnz", "brhz", "brhnz", "bi", "biz", "binz", "bihz", "bihnz", "bra", "wrch", "stop", "stopd",
         "heqi", "hgti", "hlgti", "heq", "hgt", "hlgt", "mtspr", "fscrwr", "iret", "invalid"}


def own_used(f):
    used = set()
    for pc, d in f.insns.items():
        if d["op"] not in SKIP:
            used |= regs_of(d)
    return used


def own_def(f):
    defs = set()
    for pc, d in f.insns.items():
        if d["op"] not in SKIP and d["op"] not in NODEF and "rt" in d:
            defs.add(d["rt"])
    return defs


from rj_kernels import KERNELS

TU = {}
TD = {}
SPEC = {}
DIRECT = os.environ.get("RJ_DIRECT", "1") == "1"


def closure(funcs):
    for e, f in funcs.items():
        TU[e] = own_used(f)
        TD[e] = own_def(f)
    changed = True
    while changed:
        changed = False
        for e, f in funcs.items():
            u = set(TU[e])
            dd = set(TD[e])
            for c in f.calls | f.tails:
                u |= TU[c]
                dd |= TD[c]
            if u != TU[e] or dd != TD[e]:
                TU[e] = u
                TD[e] = dd
                changed = True


def emit_func(f, out):
    sig_index = len(out)
    out.append("%svoid f_%05x(V* __restrict R, uint8_t* __restrict ls, Ctl& ctl) {" % (SPEC.get(f.entry, ""), f.entry))
    pcs = sorted(f.insns)
    used_set = own_used(f) | ({0} if f.exits else set())
    used = sorted(used_set)
    defs = sorted(own_def(f))
    direct = DIRECT
    spill = "" if direct else " ".join("R[%d] = r%d;" % (r, r) for r in defs)

    def call_spill(c, tail):
        if direct:
            return ""
        regs = (TU[c] & used_set) | (set(defs) if tail else set())
        return " ".join("R[%d] = r%d;" % (r, r) for r in sorted(regs))

    def call_fill(c):
        if direct:
            return ""
        return " ".join("r%d = R[%d];" % (r, r) for r in sorted(TD[c] & used_set))
    if direct:
        out.append("    " + " ".join("V& r%d = R[%d];" % (r, r) for r in used))
    else:
        out.append("    " + " ".join("V r%d = R[%d];" % (r, r) for r in used))
    if f.multi:
        out[sig_index] = out[sig_index].replace("Ctl& ctl) {", "Ctl& ctl, uint32_t entry) {")
        out.append("    switch (entry) { %s default: FAIL(entry); }" % " ".join("case 0x%05x: goto L_%05x;" % (e, e) for e in f.multi))
    elif pcs[0] != f.entry:
        out.append("    goto L_%05x;" % f.entry)
        f.labels.add(f.entry)
    f.loops = set()
    for p0, d0 in f.insns.items():
        if d0["op"] in ("br", "brz", "brnz", "brhz", "brhnz"):
            t0 = (p0 + d0["imm"] * 4) & 0x3FFFC
            if t0 <= p0 and t0 in f.insns:
                f.loops.add(t0)
    prev = None
    body = []
    for pc in pcs:
        if prev is not None and pc != prev + 4 and f.insns[prev]["op"] not in ("br", "bi", "bra") and f.insns[prev]["op"] not in FAILOPS - {"rdch", "wrch", "rchcnt"}:
            body.append(("goto", prev + 4))
            f.labels.add(prev + 4)
        prev = pc
        body.append(("insn", pc))
    if prev is not None and f.insns[prev]["op"] not in ("br", "bi", "bra") and f.insns[prev]["op"] not in FAILOPS - {"rdch", "wrch", "rchcnt"}:
        body.append(("goto", prev + 4))
    for kind, pc in body:
        if kind == "goto":
            if pc in f.insns or pc in f.exits:
                out.append("    goto L_%05x;" % pc)
            else:
                out.append("    FAIL(0x%05x);" % pc)
            continue
        if pc in f.labels:
            out.append("L_%05x:" % pc)
            if pc in KERNELS:
                out.append("    " + KERNELS[pc])
                out.append("L_%05x_b:" % pc)
            if pc in f.loops:
                out.append("    if (--ctl.budget == 0 || ctl.fail) FAIL(0x%05x);" % pc)
        d = f.insns[pc]
        op = d["op"]
        rt = d.get("rt")
        ra = d.get("ra")
        rb = d.get("rb")
        rc = d.get("rc")
        imm = d.get("imm")
        s = None
        if op in SKIP:
            continue
        if op in FAILOPS:
            s = "FAIL(@PC@);"
        elif op in HALTS:
            s = "if (" + HALTS[op] % d + ") FAIL(@PC@);"
        elif op in RR3:
            s = "R[%d] = %s(R[%d], R[%d]);" % (rt, cn(op), ra, rb)
        elif op in RR2:
            s = "R[%d] = %s(R[%d]);" % (rt, cn(op), ra)
        elif op in RI:
            s = "R[%d] = %s(R[%d], %d);" % (rt, cn(op), ra, imm)
        elif op in ("selb", "shufb", "fma", "fms", "fnms", "mpya"):
            s = "R[%d] = %s(R[%d], R[%d], R[%d]);" % (rt, op, ra, rb, rc)
        elif op in ("addx", "sfx", "cgx", "bgx"):
            s = "R[%d] = %s(R[%d], R[%d], R[%d]);" % (rt, op, ra, rb, rt)
        elif op in ("cflts", "cfltu", "cuflt", "csflt"):
            s = "R[%d] = %s(R[%d], %d);" % (rt, op, ra, imm)
        elif op == "il":
            s = "R[%d] = il(%d);" % (rt, imm)
        elif op == "ilh":
            s = "R[%d] = ilh(0x%x);" % (rt, d["u16"])
        elif op == "ilhu":
            s = "R[%d] = ilhu(0x%x);" % (rt, d["u16"])
        elif op == "iohl":
            s = "R[%d] = iohl(R[%d], 0x%x);" % (rt, rt, d["u16"])
        elif op == "ila":
            s = "R[%d] = ila(0x%x);" % (rt, imm)
        elif op == "fsmbi":
            s = "R[%d] = fsmbi(0x%x);" % (rt, d["u16"])
        elif op == "lqd":
            s = "R[%d] = lq(ls, pref(R[%d]) + %d);" % (rt, ra, imm * 16)
        elif op == "lqx":
            s = "R[%d] = lq(ls, pref(R[%d]) + pref(R[%d]));" % (rt, ra, rb)
        elif op == "lqr":
            s = "R[%d] = lq(ls, 0x%x);" % (rt, (pc + imm * 4) & 0x3FFFF)
        elif op == "lqa":
            s = "R[%d] = lq(ls, 0x%x);" % (rt, (imm * 4) & 0x3FFFF)
        elif op == "stqd":
            s = "st(ls, pref(R[%d]) + %d, R[%d], ctl);" % (ra, imm * 16, rt)
        elif op == "stqx":
            s = "st(ls, pref(R[%d]) + pref(R[%d]), R[%d], ctl);" % (ra, rb, rt)
        elif op == "stqr":
            s = "st(ls, 0x%x, R[%d], ctl);" % ((pc + imm * 4) & 0x3FFFF, rt)
        elif op == "stqa":
            s = "st(ls, 0x%x, R[%d], ctl);" % ((imm * 4) & 0x3FFFF, rt)
        elif op == "br":
            t = (pc + imm * 4) & 0x3FFFC
            if t in f.tails:
                s = "@TAIL%05x@" % t
            else:
                s = "goto L_%05x%s;" % (t, "_b" if t in KERNELS and t <= pc else "")
        elif op in ("brz", "brnz", "brhz", "brhnz"):
            t = (pc + imm * 4) & 0x3FFFC
            cond = {"brz": "pref(R[%d]) == 0", "brnz": "pref(R[%d]) != 0", "brhz": "hw6(R[%d]) == 0", "brhnz": "hw6(R[%d]) != 0"}[op] % rt
            s = "if (%s) goto L_%05x%s;" % (cond, t, "_b" if t in KERNELS and t <= pc else "")
        elif op == "brsl":
            t = (pc + imm * 4) & 0x3FFFC
            if t == pc + 4:
                s = "R[%d] = pref_only(0x%x);" % (rt, pc + 4)
            else:
                s = "R[%d] = pref_only(0x%x); @CALL%05x@" % (rt, pc + 4, t)
        elif op in ("bisl", "bra", "brasl"):
            s = "FAIL(@PC@);"
        elif op == "bi" and pc in getattr(f, "switches", {}):
            s = "switch (pref(R[%d]) & 0x3fffc) { %s default: FAIL(@PC@); }" % (ra, " ".join("case 0x%05x: goto L_%05x;" % (t, t) for t in sorted(set(f.switches[pc]))))
        elif op == "bi":
            s = "@RET@" if ra == 0 and d["rb"] & 0x30 == 0 else "FAIL(@PC@);"
        elif op in ("biz", "binz", "bihz", "bihnz"):
            cond = {"biz": "pref(R[%d]) == 0", "binz": "pref(R[%d]) != 0", "bihz": "hw6(R[%d]) == 0", "bihnz": "hw6(R[%d]) != 0"}[op] % rt
            s = "if (%s) %s" % (cond, "@RET@" if ra == 0 and d["rb"] & 0x30 == 0 else "FAIL(@PC@);")
        else:
            raise Exception("unhandled %s at %05x" % (op, pc))
        s = s.replace("@PC@", "0x%05x" % pc)
        s = re.sub(r"R\[(\d+)\]", lambda m: "r" + m.group(1), s)
        s = s.replace("@RET@", "{ " + spill + " return; }")
        s = re.sub(r"@CALL([0-9a-f]+)@", lambda m: "{ " + call_spill(int(m.group(1), 16), False) + " f_" + m.group(1) + "(R, ls, ctl); if (ctl.fail) return; " + call_fill(int(m.group(1), 16)) + " }", s)
        s = re.sub(r"@TAIL([0-9a-f]+)@", lambda m: "{ " + call_spill(int(m.group(1), 16), True) + " f_" + m.group(1) + "(R, ls, ctl); return; }", s)
        out.append("    %s" % s)
    for x in sorted(f.exits):
        out.append("L_%05x:" % x)
        if r0_dead(x):
            out.append("    r0 = pref_only(0x%05x);" % x)
            out.append("    { " + ("" if DIRECT else " ".join("R[%d] = r%d;" % (r, r) for r in sorted(set(defs) | {0}))) + " return; }")
        else:
            out.append("    FAIL(0x%05x);" % x)
            print("warning: r0 live at exit %05x" % x, file=sys.stderr)
    out.append("}")
    out.append("")


def all_entries():
    roots = set([0x14ae0, 0x14cf8])
    for pc in range(IMG_LS + 0x30, 0x17300, 4):
        d = decode(pc)
        if d["op"] == "brsl" and d["rt"] == 0:
            t = (pc + d["imm"] * 4) & 0x3FFFC
            if t != pc + 4:
                roots.add(t)
    return sorted(roots)


def ops_mode(outp, hdr):
    seen = {}
    for pc in range(IMG_LS + 0x30, 0x17300, 4):
        d = decode(pc)
        op = d["op"]
        if op in SKIP or op in FAILOPS or op in HALTS or op.startswith("b") and op not in ("bg", "bgx") or op in ("brsl", "bra", "brasl", "iret"):
            continue
        key = (op, d.get("imm") if op in ("cflts", "cfltu", "cuflt", "csflt") else None)
        if key not in seen:
            seen[key] = pc
    funcs = []
    for key, pc in sorted(seen.items(), key=lambda x: x[1]):
        f = Func(pc)
        f.insns[pc] = decode(pc)
        f.insns[pc + 4] = dict(op="bi", rt=0, ra=0, rb=0, w=0)
        funcs.append(f)
    out = ['#include "%s"' % os.path.basename(hdr), "", "namespace renjob::opz {", "", "#define FAIL(pc) do { ctl.fail = true; ctl.fail_pc = pc; return; } while (0)", ""]
    for f in funcs:
        TU[f.entry] = set()
        TD[f.entry] = set()
        lines = []
        emit_func(f, lines)
        out += lines
    out += ["#undef FAIL", "", "}"]
    open(outp, "w", newline="\n").write("\n".join(out) + "\n")
    h = ["#pragma once", "", '#include "renjob_ops.h"', "", "namespace renjob::opz {", "", "using namespace renjob::xo;", ""]
    for f in funcs:
        h.append("void f_%05x(V* R, uint8_t* ls, Ctl& ctl);" % f.entry)
    h.append("struct OpEntry { uint32_t lsa; const char* name; void (*fn)(V*, uint8_t*, Ctl&); };")
    h.append("inline constexpr OpEntry kOps[] = {%s};" % ", ".join('{0x%05x, "%s", f_%05x}' % (f.entry, f.insns[f.entry]["op"], f.entry) for f in funcs))
    h += ["", "}"]
    open(hdr, "w", newline="\n").write("\n".join(h) + "\n")
    print(len(funcs), "ops")


def main():
    if sys.argv[1] == "ops":
        ops_mode(sys.argv[2], sys.argv[3])
        return
    froots = []
    regions = {}
    spec = sys.argv[1]
    if spec.startswith("@"):
        spec = open(spec[1:]).read().strip()
    for tok in spec.split(","):
        if ":" in tok:
            a, b = tok.split(":")
            regions[int(a, 16)] = frozenset(int(x, 16) for x in b.split("/") if x)
        else:
            froots.append(int(tok, 16))
    outp = sys.argv[2]
    hdr = sys.argv[3]
    ns = sys.argv[4] if len(sys.argv) > 4 else "renjob::xo"
    table = len(sys.argv) > 5
    if froots == [0]:
        froots = all_entries()
    froots = [r for r in froots if r not in regions]
    funcs = {}
    regs = {}
    known = set(froots)
    changed = True
    fams = {}
    for r in sorted(regions):
        fams.setdefault(regions[r], []).append(r)
    while changed:
        changed = False
        regs = {}
        for st, starts in fams.items():
            g = discover(starts[0], known, st, starts)
            for r in starts:
                regs[r] = g
        for g in regs.values():
            for c in g.calls | g.tails:
                if c not in known:
                    known.add(c)
                    changed = True
        for e in sorted(known):
            if e in funcs:
                continue
            funcs[e] = discover(e, known)
            for c in funcs[e].calls | funcs[e].tails:
                if c not in known:
                    known.add(c)
                    changed = True
        if changed:
            funcs = {}
    base = os.path.basename(hdr)
    out = ['#include "%s"' % base, "", "namespace %s {" % ns, "", "#define FAIL(pc) do { ctl.fail = true; ctl.fail_pc = pc; return; } while (0)", ""]
    inline_max = int(os.environ.get("RJ_INLINE_MAX", "120"))
    for e in sorted(funcs):
        if e not in froots and not table and len(funcs[e].insns) <= inline_max and not (funcs[e].calls & set(funcs)) - set([0x14f40]):
            SPEC[e] = "static inline __attribute__((always_inline)) "
    for e in sorted(funcs):
        out.append("%svoid f_%05x(V* R, uint8_t* ls, Ctl& ctl);" % (SPEC.get(e, ""), e))
    out.append("")
    closure(funcs)
    for e in sorted(funcs):
        emit_func(funcs[e], out)
    done = set()
    for e in sorted(regs):
        g = regs[e]
        if id(g) in done:
            continue
        done.add(id(g))
        lines = []
        emit_func(g, lines)
        name = ("m_%05x" if g.multi else "g_%05x") % g.entry
        lines[0] = lines[0].replace("static inline __attribute__((always_inline)) ", "").replace("void f_%05x(" % g.entry, "void %s(" % name)
        if g.multi:
            lines[0] = "static " + lines[0]
            out += lines
            for r in g.multi:
                out.append("void g_%05x(V* R, uint8_t* ls, Ctl& ctl) { %s(R, ls, ctl, 0x%05x); }" % (r, name, r))
            out.append("")
        else:
            out += lines
    out.append("#undef FAIL")
    out.append("")
    out.append("}")
    open(outp, "w", newline="\n").write("\n".join(out) + "\n")
    h = ["#pragma once", "", '#include "renjob_ops.h"', "", "namespace %s {" % ns, "", "using namespace renjob::xo;", ""]
    for e in sorted(froots if not table else funcs):
        h.append("void f_%05x(V* R, uint8_t* ls, Ctl& ctl);" % e)
    for e in sorted(regs):
        h.append("void g_%05x(V* R, uint8_t* ls, Ctl& ctl);" % e)
    h.append("")
    allroots = sorted(set(froots) | set(regs))
    for e in allroots:
        h.append("inline constexpr uint32_t kSig%05x[16] = {%s};" % (e, ", ".join("0x%08xu" % word(e + 4 * i) for i in range(16))))
    h.append("")
    h.append("#define RJ_ROOTS(X) " + " ".join("X(%05x, %s_%05x)" % (e, "g" if e in regs else "f", e) for e in allroots))
    if table:
        h.append("")
        h.append("struct GenEntry { uint32_t lsa; void (*fn)(V*, uint8_t*, Ctl&); };")
        h.append("inline constexpr GenEntry kAllGen[] = {%s};" % ", ".join("{0x%05x, f_%05x}" % (e, e) for e in sorted(funcs)))
    h += ["", "}"]
    open(hdr, "w", newline="\n").write("\n".join(h) + "\n")
    for e in sorted(funcs):
        print("%05x insns %d calls %s tails %s" % (e, len(funcs[e].insns), sorted("%05x" % c for c in funcs[e].calls), sorted("%05x" % c for c in funcs[e].tails)))
    for e in sorted(regs):
        print("region %05x insns %d exits %s family %s" % (e, len(regs[e].insns), sorted("%05x" % c for c in regs[e].exits), "%05x" % regs[e].entry))


if __name__ == "__main__":
    main()
