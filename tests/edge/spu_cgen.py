import os
import re
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import spu_dis as D

ELF = os.path.join(ROOT, "bin", "01.12", "GOWA.patched.elf")
IMG_OFF = 0x727580
IMG_LS = 0x4000
IMG_SIZE = 0xC430

data = open(ELF, "rb").read()
img = data[IMG_OFF:IMG_OFF + IMG_SIZE]


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
    raise Exception("bad insn %05x %08x" % (pc, w))


SKIP = {"hbrr", "hbr", "hbra", "nop", "lnop", "heqi", "sync", "dsync"}
RR3 = {"a", "ah", "sf", "sfh", "and", "andc", "or", "xor", "nor", "nand", "orc", "eqv", "cg", "mpy", "mpyu", "mpyh", "mpyhhu", "mpyhh",
       "shl", "rot", "rotm", "rotma", "roth", "rothm", "rotmah", "ceq", "ceqh", "ceqb", "cgt", "cgth", "cgtb", "clgt", "clgth", "clgtb",
       "shlqby", "rotqby", "rotqmby", "fa", "fs", "fm", "fcgt", "fceq", "fcmgt", "fcmeq", "fi"}
RR2 = {"cntb", "xsbh", "xshw", "xswd", "fsmb", "fsmh", "fsm", "gb", "gbh", "frest", "frsqest"}
RI = {"ai", "ahi", "sfi", "sfhi", "andi", "andhi", "andbi", "ori", "orhi", "orbi", "xori", "xorhi", "xorbi", "mpyi", "mpyui",
      "ceqi", "ceqhi", "ceqbi", "cgti", "cgthi", "cgtbi", "clgti", "clgthi", "clgtbi",
      "shli", "shlhi", "roti", "rotmi", "rotmai", "rothmi", "rotmahi", "shlqbyi", "rotqbyi", "rotqmbyi", "shlqbii", "cbd", "chd", "cwd", "cdd"}
CPPNAME = {"and": "and_", "or": "or_", "xor": "xor_"}


def cn(op):
    return CPPNAME.get(op, op)


class Func:
    def __init__(self, entry):
        self.entry = entry
        self.insns = {}
        self.labels = set()
        self.calls = set()
        self.tails = set()


def discover(entry, known_entries):
    f = Func(entry)
    work = [entry]
    while work:
        pc = work.pop()
        while pc not in f.insns:
            d = decode(pc)
            f.insns[pc] = d
            op = d["op"]
            nxt = pc + 4
            if op in ("br",):
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
            if op == "bi":
                if d["ra"] != 0:
                    raise Exception("computed jump at %05x" % pc)
                break
            pc = nxt
    return f


def regs_of(d):
    rs = set()
    for k in ("rt", "ra", "rb", "rc"):
        if k in d:
            rs.add(d[k])
    return rs


def emit_func(f, out):
    out.append("void f_%05x(V* R, uint8_t* ls, Ctl& ctl) {" % f.entry)
    pcs = sorted(f.insns)
    used = set()
    for pc in pcs:
        if f.insns[pc]["op"] not in SKIP:
            used |= regs_of(f.insns[pc])
    used = sorted(used)
    spill = " ".join("R[%d] = r%d;" % (r, r) for r in used)
    fill = " ".join("r%d = R[%d];" % (r, r) for r in used)
    out.append("    " + " ".join("V r%d = R[%d];" % (r, r) for r in used))
    prev = None
    for pc in pcs:
        if prev is not None and pc != prev + 4 and decode(prev)["op"] not in ("br", "bi"):
            raise Exception("gap after %05x" % prev)
        prev = pc
        if pc in f.labels:
            out.append("L_%05x:" % pc)
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
        if op in RR3:
            s = "R[%d] = %s(R[%d], R[%d]);" % (rt, cn(op), ra, rb)
        elif op in RR2:
            s = "R[%d] = %s(R[%d]);" % (rt, cn(op), ra)
        elif op in RI:
            s = "R[%d] = %s(R[%d], %d);" % (rt, cn(op), ra, imm)
        elif op in ("selb", "shufb", "fma", "fms", "fnms"):
            s = "R[%d] = %s(R[%d], R[%d], R[%d]);" % (rt, op, ra, rb, rc)
        elif op == "addx":
            s = "R[%d] = addx(R[%d], R[%d], R[%d]);" % (rt, ra, rb, rt)
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
            s = "sq(ls, pref(R[%d]) + %d, R[%d]);" % (ra, imm * 16, rt)
        elif op == "stqx":
            s = "sq(ls, pref(R[%d]) + pref(R[%d]), R[%d]);" % (ra, rb, rt)
        elif op == "stqr":
            s = "sq(ls, 0x%x, R[%d]);" % ((pc + imm * 4) & 0x3FFFF, rt)
        elif op == "stqa":
            s = "sq(ls, 0x%x, R[%d]);" % ((imm * 4) & 0x3FFFF, rt)
        elif op == "br":
            t = (pc + imm * 4) & 0x3FFFC
            if t in f.tails:
                s = "@CALL%05x@ @RET@" % t
            else:
                s = "goto L_%05x;" % t
        elif op in ("brz", "brnz", "brhz", "brhnz"):
            t = (pc + imm * 4) & 0x3FFFC
            cond = {"brz": "pref(R[%d]) == 0", "brnz": "pref(R[%d]) != 0", "brhz": "hw6(R[%d]) == 0", "brhnz": "hw6(R[%d]) != 0"}[op] % rt
            s = "if (%s) goto L_%05x;" % (cond, t)
        elif op == "brsl":
            t = (pc + imm * 4) & 0x3FFFC
            if t == pc + 4:
                s = "R[%d] = pref_only(0x%x);" % (rt, pc + 4)
            else:
                s = "R[%d] = pref_only(0x%x); @CALL%05x@" % (rt, pc + 4, t)
        elif op == "bisl":
            s = "R[%d] = pref_only(0x%x); ctl.fail = true; @RET@" % (rt, pc + 4)
        elif op == "bi":
            s = "@RET@"
        elif op == "biz":
            s = "if (pref(R[%d]) == 0) @RET@" % rt
        elif op == "binz":
            s = "if (pref(R[%d]) != 0) @RET@" % rt
        else:
            raise Exception("unhandled %s at %05x" % (op, pc))
        s = re.sub(r"R\[(\d+)\]", lambda m: "r" + m.group(1), s)
        s = s.replace("@RET@", "{ " + spill + " return; }")
        s = re.sub(r"@CALL([0-9a-f]+)@", lambda m: "{ " + spill + " f_" + m.group(1) + "(R, ls, ctl); if (ctl.fail) return; " + fill + " }", s)
        out.append("    %s" % s)
    out.append("}")
    out.append("")


def main():
    roots = [int(x, 16) for x in sys.argv[1].split(",")]
    outp = sys.argv[2]
    funcs = {}
    known = set(roots)
    changed = True
    while changed:
        changed = False
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
    out = ['#include "edge_cull_gen.h"', "", "namespace edge::xo {", ""]
    for e in sorted(funcs):
        out.append("void f_%05x(V* R, uint8_t* ls, Ctl& ctl);" % e)
    out.append("")
    for e in sorted(funcs):
        emit_func(funcs[e], out)
    out.append("}")
    open(outp, "w").write("\n".join(out) + "\n")
    for e in sorted(funcs):
        print("%05x insns %d calls %s tails %s" % (e, len(funcs[e].insns), sorted("%05x" % c for c in funcs[e].calls), sorted("%05x" % c for c in funcs[e].tails)))


main()
