import os
import re
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import spu_dis as D

ELF = (os.environ.get("GOWA_ELF") or os.path.join(ROOT, "bin", "01.12", "GOWA.patched.elf"))
IMG_EA = 0x705680
IMG_LS = 0x4000
IMG_SIZE = 0x13580

CODE_END = 0x17300
ENTRY_ROOTS = (0x14ae0, 0x14cf8)
INLINE_EXCEPT = (0x14f40,)
OPS_INCLUDE = "renjob_ops.h"
BASE_NS = "renjob::xo"
OPS_NS = "renjob::opz"
ROOTS_MACRO = "RJ_ROOTS"
ICALLS = {}
RECURSION_LIMIT = 0
WHOLE = False
WHOLE_ROOTS = (0x14cf8, 0x052c8, 0x13310, 0x14ae0, 0x14800, 0x05830)
WHOLE_MULTI = {0x14cf8: (0x14cf8, 0x14da4)}
PM_CALLS = {0x017b8: "pm_gettag(R, ls, ctl);", 0x017c0: "pm_releasetag(R, ls, ctl);", 0x017e0: ""}
LOOP_BUDGET = False
WHOLE_NS = "renjob::wh"
WHOLE_PM = False
WHOLE_RETCHECK = False
WHOLE_HDR_EXTRA = ()
WHOLE_PM_DECLS = True
JUMP_ABS = False
KERNEL_IMG = None
WHOLE_ICALL_DEFAULT = "ctl.icall_miss = t; FAIL(pc);"
PM_EA = 0x7A7B00
PM_LS = 0xA00
PM_SIZE = 0x2FF0
LINK = {}
DOPS3 = ()
REC_EDGES = set()
PC_COUNTS_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pc_counts.txt")

data = open(ELF, "rb").read()
img = data[IMG_EA - 0x10000:IMG_EA - 0x10000 + IMG_SIZE]


def configure(**kw):
    global img
    g = globals()
    for k, v in kw.items():
        assert k in g, k
        g[k] = v
    img = data[IMG_EA - 0x10000:IMG_EA - 0x10000 + IMG_SIZE]


def word(pc):
    if WHOLE_PM and pc < PM_LS:
        return struct.unpack_from(">I", KERNEL_IMG, pc)[0] if KERNEL_IMG and pc + 4 <= len(KERNEL_IMG) else 0
    if WHOLE_PM and PM_LS <= pc < IMG_LS:
        return struct.unpack_from(">I", data, PM_EA - 0x10000 + pc - PM_LS)[0] if pc < PM_LS + PM_SIZE else 0
    if WHOLE_PM and not (IMG_LS <= pc < IMG_LS + len(img)):
        return 0
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
        out.append(v if JUMP_ABS else base + v)
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
        self.has_fail = False


def ie_idiom(pc):
    d = decode(pc)
    if d["op"] not in ("bi", "biz", "binz", "bihz", "bihnz") or d["ra"] == 0:
        return False
    a, b = decode(pc - 8), decode(pc - 4)
    if WHOLE_PM and b["op"] == "ila" and b["rt"] == d["ra"] and (b["imm"] & 0x3FFFC) == pc + 4 and (d["rb"] >> 4) & 3 in (1, 2):
        return True
    return a["op"] == "brsl" and a["rt"] == d["ra"] and ((pc - 8 + a["imm"] * 4) & 0x3FFFC) == pc - 4 and b["op"] == "ai" and b["rt"] == d["ra"] and b["ra"] == d["ra"] and b["imm"] == 8


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
            if WHOLE and ie_idiom(pc):
                pc = nxt
                continue
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
                if t != nxt and t not in NOCALL:
                    f.calls.add(t)
                    if WHOLE and d["rt"] != 0:
                        LINK.setdefault(t, set()).add(d["rt"])
            if op == "bisl" and pc in ICALLS and d["rb"] & 0x30 == 0:
                for t in ICALLS[pc]:
                    if t not in NOCALL:
                        f.calls.add(t)
            if op == "bi" and d["ra"] != 0 and d["rb"] & 0x30 == 0 and jump_table(pc):
                f.switches = getattr(f, "switches", {})
                f.switches[pc] = jump_table(pc)
                for t in f.switches[pc]:
                    f.labels.add(t)
                    work.append(t)
                break
            if WHOLE_PM and op == "ila":
                f.ila = getattr(f, "ila", {})
                f.ila.setdefault(d["rt"], set()).add(d["imm"] & 0x3FFFC)
            if WHOLE_PM and op == "bi" and (d["rb"] >> 4) & 3 != 3 and not ie_idiom(pc):
                f.ibr = getattr(f, "ibr", {})
                f.ibr[pc] = d["ra"]
            if op in ("bi", "bra", "brasl") or op in FAILOPS - {"rdch", "wrch", "rchcnt", "mfspr", "mtspr", "fscrrd", "fscrwr"}:
                break
            pc = nxt
        if not work and WHOLE_PM:
            for bpc, ra in getattr(f, "ibr", {}).items():
                for t in sorted(getattr(f, "ila", {}).get(ra, ())):
                    if t not in f.insns and t not in work and IMG_LS > t >= PM_LS:
                        f.labels.add(t)
                        work.append(t)
                    elif t in f.insns:
                        f.labels.add(t)
    return f


def reads_of(d):
    op = d["op"]
    rs = set()
    if op in ("stqd", "stqx", "stqr", "stqa", "wrch", "brz", "brnz", "brhz", "brhnz", "biz", "binz", "bihz", "bihnz") and "rt" in d:
        rs.add(d["rt"])
    if op in ("selb", "shufb", "fma", "fms", "fnms", "mpya"):
        rs |= {d["ra"], d["rb"], d["rc"]}
        return rs
    if op in ("addx", "sfx", "cgx", "bgx", "iohl") or op in DOPS3:
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
EXITS = os.environ.get("RJ_EXITS", "1") == "1"
PROF = os.environ.get("RJ_PROF", "0") == "1"
PROMOTE = os.environ.get("RJ_PROMOTE", "1") == "1"
LEAF_LOCALS = os.environ.get("RJ_LEAF_LOCALS", "0") == "1"
HINTS = os.environ.get("RJ_HINTS", "1") == "1"
BUDGET = False
NOCALL = set()


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


ABI_RE = re.compile(r"^((?:static )?(?:inline __attribute__\(\(always_inline\)\) )?)void ([fgm]_[0-9a-f]{5}\()")


def abi(line):
    return ABI_RE.sub(r"\1RJ_ABI void \2", line)


PC_COUNTS = None


def pc_counts():
    global PC_COUNTS
    if PC_COUNTS is None:
        PC_COUNTS = {}
        path = PC_COUNTS_PATH
        if HINTS and path and os.path.exists(path):
            for line in open(path):
                a, b = line.split()
                PC_COUNTS[int(a, 16)] = int(b)
    return PC_COUNTS


def branch_hint(f, pc):
    cnt = pc_counts()
    ex = cnt.get(pc, 0)
    if ex < 64 or (pc + 4) in f.labels:
        return None
    fall = min(cnt.get(pc + 4, 0), ex)
    taken = ex - fall
    if taken * 20 <= ex:
        return 0
    if fall * 20 <= ex:
        return 1
    return None


def find_promotable(f):
    cands = {}
    for p0, d0 in f.insns.items():
        if d0["op"] in ("br", "brz", "brnz", "brhz", "brhnz"):
            t0 = (p0 + d0["imm"] * 4) & 0x3FFFC
            if t0 <= p0 and t0 in f.insns:
                cands[t0] = max(cands.get(t0, 0), p0)
    res = {}
    for h in sorted(cands):
        hi = cands[h]
        ok = True
        regs = set()
        for q in range(h, hi + 4, 4):
            d = f.insns.get(q)
            if d is None:
                ok = False
                break
            op = d["op"]
            if op in ("brsl", "bi", "bisl", "biz", "binz", "bihz", "bihnz", "bra", "brasl", "iret") or op in FAILOPS or op in HALTS or op == "invalid":
                ok = False
                break
            if q in KERNELS and q != h:
                ok = False
                break
            if op not in SKIP:
                regs |= regs_of(d)
        if not ok:
            continue
        for p0, d0 in f.insns.items():
            if h <= p0 <= hi:
                continue
            if d0["op"] in ("br", "brz", "brnz", "brhz", "brhnz"):
                t0 = (p0 + d0["imm"] * 4) & 0x3FFFC
                if h < t0 <= hi:
                    ok = False
                    break
        for sw in getattr(f, "switches", {}).values():
            if any(h <= t <= hi for t in sw):
                ok = False
        if f.multi and any(h < e <= hi for e in f.multi):
            ok = False
        if h < f.entry <= hi or any(h < t <= hi for ts in getattr(f, "ila", {}).values() for t in ts):
            ok = False
        if any(h < x <= hi for x in f.exits):
            ok = False
        if not ok:
            continue
        if any(not (hi < lo2 or h > hi2) for lo2, (lo2b, hi2, _) in [(k, v) for k, v in res.items()]):
            continue
        if f.insns[hi]["op"] != "br":
            if hi + 4 in f.insns:
                f.labels.add(hi + 4)
            elif hi + 4 not in f.exits:
                continue
        res[h] = (h, hi, sorted(regs))
    return res


def ie_act(d):
    fl = (d["rb"] >> 4) & 3
    return "*ctl.ie = true; " if fl == 1 else "*ctl.ie = false; " if fl == 2 else ""


def retcheck(rt, ret):
    r = "r%s" % rt if str(rt).isdigit() else rt
    return "if (__builtin_expect((pref(%s) & 0x3fffc) != 0x%x, 0)) { ctl.fail = true; ctl.out = true; ctl.fail_pc = pref(%s) & 0x3fffc; return; }" % (r, ret, r)


def xf(f, pc):
    if EXITS and r0_dead(pc):
        return "EXIT(0x%05x)" % pc
    f.has_fail = True
    return "FAIL(0x%05x)" % pc


def emit_func(f, out):
    sig_index = len(out)
    out.append("%svoid f_%05x(V* __restrict R, uint8_t* __restrict ls, Ctl& ctl) {" % (SPEC.get(f.entry, ""), f.entry))
    pcs = sorted(f.insns)
    used_set = own_used(f) | ({0} if f.exits else set())
    used = sorted(used_set)
    defs = sorted(own_def(f))
    direct = DIRECT and not (LEAF_LOCALS and not (f.calls | f.tails))
    f.direct = direct
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
    if PROF:
        out.append("    rjprof::Scope rj_prof_scope(0x%05x);" % f.entry)
    if direct:
        out.append("    " + " ".join("V& r%d = R[%d];" % (r, r) for r in used))
    else:
        out.append("    " + " ".join("V r%d = R[%d];" % (r, r) for r in used))
    if f.multi:
        out[sig_index] = out[sig_index].replace("Ctl& ctl) {", "Ctl& ctl, uint32_t entry) {")
        out.append("    switch (entry) { %s default: __builtin_unreachable(); }" % " ".join("case 0x%05x: goto L_%05x;" % (e, e) for e in f.multi))
    elif pcs[0] != f.entry:
        out.append("    goto L_%05x;" % f.entry)
        f.labels.add(f.entry)
    f.loops = set()
    for p0, d0 in f.insns.items():
        if d0["op"] in ("br", "brz", "brnz", "brhz", "brhnz"):
            t0 = (p0 + d0["imm"] * 4) & 0x3FFFC
            if t0 <= p0 and t0 in f.insns:
                f.loops.add(t0)
    promo = find_promotable(f) if PROMOTE and direct else {}
    in_loop = {}
    for h, (lo, hi, regs) in promo.items():
        for q in range(lo, hi + 4, 4):
            in_loop[q] = h
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
                out.append("    %s;" % xf(f, pc))
            continue
        if pc in f.labels:
            out.append("L_%05x:" % pc)
            if pc in KERNELS:
                out.append("    " + KERNELS[pc])
            if pc in promo:
                out.append("    {")
                out.append("    " + " ".join("V r%d = R[%d];" % (r, r) for r in promo[pc][2]))
            if pc in KERNELS or pc in promo:
                out.append("L_%05x_b:" % pc)
            if pc in f.loops and BUDGET:
                f.has_fail = True
                out.append("    if (--ctl.budget == 0) FAIL(0x%05x);" % pc)
            elif pc in f.loops and LOOP_BUDGET and r0_dead(pc):
                wb = " ".join("R[%d] = r%d;" % (r, r) for r in promo[pc][2]) if pc in promo else ""
                out.append("    if (__builtin_expect(--ctl.budget == 0, 0)) { %s EXIT(0x%05x); }" % (wb, pc))
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
        if WHOLE and ie_idiom(pc):
            fl = (d["rb"] >> 4) & 3
            act = "*ctl.ie = true;" if fl == 1 else "*ctl.ie = false;" if fl == 2 else ""
            if op == "bi":
                s = act
            else:
                cond = {"biz": "pref(R[%d]) == 0", "binz": "pref(R[%d]) != 0", "bihz": "hw6(R[%d]) == 0", "bihnz": "hw6(R[%d]) != 0"}[op] % rt
                s = "if (%s) { %s }" % (cond, act) if act else ""
            if s:
                out.append("    %s" % re.sub(r"R\[(\d+)\]", lambda m: "r" + m.group(1), s))
            continue
        if WHOLE and op == "rdch":
            s = "R[%d] = pref_only(ctl.host->rd(%d)); if (__builtin_expect(ctl.host->abort, 0)) FAIL(@PC@);" % (rt, ra)
        elif WHOLE and op == "rchcnt":
            s = "R[%d] = pref_only(ctl.host->cnt(%d));" % (rt, ra)
        elif WHOLE and op == "wrch":
            s = "ctl.host->wr(%d, pref(R[%d])); if (__builtin_expect(ctl.host->abort, 0)) FAIL(@PC@);" % (ra, rt)
        elif WHOLE and op == "bisl" and (d["rb"] & 0x30 == 0 or WHOLE_PM and (d["rb"] >> 4) & 3 in (1, 2)) and rt != ra:
            s = "{ uint32_t it_ = pref(R[%d]); R[%d] = pref_only(0x%x); %sicall(R, ls, ctl, it_, 0x%05x); if (ctl.out) return; }" % (ra, rt, pc + 4, ie_act(d), pc)
            if WHOLE_RETCHECK:
                s = s[:-1] + " " + retcheck(rt, pc + 4) + " }"
        elif op in FAILOPS:
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
        elif op in ("addx", "sfx", "cgx", "bgx") or op in DOPS3:
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
                s = "goto L_%05x%s;" % (t, "_b" if (t in KERNELS or t in promo) and t <= pc and (t not in promo or pc in in_loop and in_loop[pc] == t) else "")
        elif op in ("brz", "brnz", "brhz", "brhnz"):
            t = (pc + imm * 4) & 0x3FFFC
            cond = {"brz": "pref(R[%d]) == 0", "brnz": "pref(R[%d]) != 0", "brhz": "hw6(R[%d]) == 0", "brhnz": "hw6(R[%d]) != 0"}[op] % rt
            hint = branch_hint(f, pc)
            if hint is not None:
                cond = "__builtin_expect(!!(%s), %d)" % (cond, hint)
            s = "if (%s) goto L_%05x%s;" % (cond, t, "_b" if (t in KERNELS or t in promo) and t <= pc and (t not in promo or pc in in_loop and in_loop[pc] == t) else "")
        elif op == "brsl":
            t = (pc + imm * 4) & 0x3FFFC
            if t == pc + 4:
                s = "R[%d] = pref_only(0x%x);" % (rt, pc + 4)
            elif t in NOCALL:
                s = "FAIL(@PC@);"
            else:
                s = "R[%d] = pref_only(0x%x); @CALL%05x@" % (rt, pc + 4, t)
        elif op == "bisl" and pc in ICALLS and d["rb"] & 0x30 == 0 and [t for t in ICALLS[pc] if t not in NOCALL]:
            cases = " ".join("case 0x%05x: R[%d] = pref_only(0x%x); @CALL%05x@ break;" % (t, rt, pc + 4, t) for t in sorted(set(ICALLS[pc])) if t not in NOCALL)
            s = "switch (pref(R[%d]) & 0x3fffc) { %s default: FAIL(@PC@); }" % (ra, cases)
        elif op in ("bisl", "bra", "brasl"):
            s = "FAIL(@PC@);"
        elif op == "bi" and pc in getattr(f, "switches", {}):
            s = "switch (pref(R[%d]) & 0x3fffc) { %s default: FAIL(@PC@); }" % (ra, " ".join("case 0x%05x: goto L_%05x;" % (t, t) for t in sorted(set(f.switches[pc]))))
        elif WHOLE and op == "bi" and ra == 0 and (d["rb"] >> 4) & 3 in (1, 2):
            s = "*ctl.ie = %s; @RET@" % ("true" if (d["rb"] >> 4) & 3 == 1 else "false")
        elif op == "bi" and WHOLE_PM and (d["rb"] >> 4) & 3 != 3 and [t for t in getattr(f, "ila", {}).get(ra, ()) if t in f.insns]:
            dflt = "@RET@" if (ra == 0 or ra in LINK.get(f.entry, ())) and d["rb"] & 0x30 == 0 else "FAIL(@PC@);"
            s = ie_act(d) + "switch (pref(R[%d]) & 0x3fffc) { %s default: %s }" % (ra, " ".join("case 0x%05x: goto L_%05x;" % (t, t) for t in sorted(t for t in f.ila[ra] if t in f.insns)), dflt)
        elif op == "bi" and WHOLE and ra != 0 and d["rb"] & 0x30 == 0 and ra in LINK.get(f.entry, ()):
            s = "@RET@"
        elif op == "bi":
            s = "@RET@" if ra == 0 and d["rb"] & 0x30 == 0 else "FAIL(@PC@);"
        elif op in ("biz", "binz", "bihz", "bihnz"):
            cond = {"biz": "pref(R[%d]) == 0", "binz": "pref(R[%d]) != 0", "bihz": "hw6(R[%d]) == 0", "bihnz": "hw6(R[%d]) != 0"}[op] % rt
            s = "if (%s) %s" % (cond, "@RET@" if ra == 0 and d["rb"] & 0x30 == 0 else "FAIL(@PC@);")
        else:
            raise Exception("unhandled %s at %05x" % (op, pc))
        if "FAIL(@PC@)" in s:
            s = s.replace("FAIL(@PC@)", xf(f, pc))
        s = re.sub(r"R\[(\d+)\]", lambda m: "r" + m.group(1), s)
        if pc in in_loop:
            h = in_loop[pc]
            lo, hi, regs = promo[h]
            wb = " ".join("R[%d] = r%d;" % (r, r) for r in regs)

            def fix_goto(m):
                t = int(m.group(1), 16)
                if m.group(2) or lo <= t <= hi:
                    return m.group(0)
                return "{ " + wb + " goto L_%05x; }" % t
            s = re.sub(r"goto L_([0-9a-f]{5})(_b)?;", fix_goto, s)
            if pc == hi:
                out.append("    %s" % s)
                if f.insns[pc]["op"] not in ("br",):
                    out.append("    { " + wb + " goto L_%05x; }" % (pc + 4))
                out.append("    }")
                continue
        s = s.replace("@RET@", "{ " + spill + " return; }")
        s = re.sub(r"@CALL([0-9a-f]+)@", lambda m: rec_call(f, pc, rt, m.group(1)) if (f.entry, int(m.group(1), 16)) in REC_EDGES and rt == 0 else "{ " + call_spill(int(m.group(1), 16), False) + " f_" + m.group(1) + "(R, ls, ctl); if (ctl.out) return; " + call_fill(int(m.group(1), 16)) + (" " + retcheck("@RT@", pc + 4) if WHOLE_RETCHECK and op in ("brsl", "bisl") else "") + " }", s)
        if "@RT@" in s:
            s = s.replace("@RT@", "r%d" % rt)
        s = re.sub(r"@TAIL([0-9a-f]+)@", lambda m: "{ " + call_spill(int(m.group(1), 16), True) + " f_" + m.group(1) + "(R, ls, ctl); return; }", s)
        out.append("    %s" % s)
    for x in sorted(f.exits):
        out.append("L_%05x:" % x)
        if r0_dead(x):
            out.append("    EXIT(0x%05x);" % x)
        else:
            out.append("    %s;" % xf(f, x))
            print("warning: r0 live at exit %05x" % x, file=sys.stderr)
    out.append("}")
    out.append("")
    if not direct:
        allsp = " ".join("R[%d] = r%d;" % (r, r) for r in defs)
        for i in range(sig_index, len(out)):
            out[i] = re.sub(r"EXIT\((0x[0-9a-f]+)\)", lambda m: "{ " + allsp + " R[0] = pref_only(" + m.group(1) + "); ctl.out = true; return; }", out[i])


def rec_call(f, pc, rt, callee):
    return "{ if (rj_depth >= %d) %s; ++rj_depth; f_%s(R, ls, ctl); --rj_depth; if (ctl.out) return; }" % (RECURSION_LIMIT, xf(f, pc), callee)


def rec_edges(funcs):
    out = set()
    for e, f in funcs.items():
        for c in f.calls | f.tails:
            seen = set()
            work = [c]
            while work:
                x = work.pop()
                if x in seen or x not in funcs:
                    continue
                seen.add(x)
                work.extend(funcs[x].calls | funcs[x].tails)
            if e in seen:
                out.add((e, c))
    return out


def all_entries():
    roots = set(ENTRY_ROOTS)
    for pc in range(IMG_LS + 0x30, CODE_END, 4):
        d = decode(pc)
        if d["op"] == "brsl" and d["rt"] == 0:
            t = (pc + d["imm"] * 4) & 0x3FFFC
            if t != pc + 4:
                roots.add(t)
    return sorted(roots)


def ops_mode(outp, hdr):
    seen = {}
    for pc in range(IMG_LS + 0x30, CODE_END, 4):
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
    out = ['#include "%s"' % os.path.basename(hdr), "", "namespace %s {" % OPS_NS, "", "#define FAIL(pc) do { ctl.fail = true; ctl.out = true; ctl.fail_pc = pc; return; } while (0)", "#define EXIT(pc) do { R[0] = pref_only(pc); ctl.out = true; return; } while (0)", ""]
    for f in funcs:
        TU[f.entry] = set()
        TD[f.entry] = set()
        lines = []
        emit_func(f, lines)
        out += lines
    out += ["#undef FAIL", "#undef EXIT", "", "}"]
    open(outp, "w", newline="\n").write("\n".join(out) + "\n")
    h = ["#pragma once", "", '#include "%s"' % OPS_INCLUDE, "", "namespace %s {" % OPS_NS, "", "using namespace %s;" % BASE_NS, ""]
    for f in funcs:
        h.append("void f_%05x(V* R, uint8_t* ls, Ctl& ctl);" % f.entry)
    h.append("struct OpEntry { uint32_t lsa; const char* name; void (*fn)(V*, uint8_t*, Ctl&); };")
    h.append("inline constexpr OpEntry kOps[] = {%s};" % ", ".join('{0x%05x, "%s", f_%05x}' % (f.entry, f.insns[f.entry]["op"], f.entry) for f in funcs))
    h += ["", "}"]
    open(hdr, "w", newline="\n").write("\n".join(h) + "\n")
    print(len(funcs), "ops")


def whole_main(outp, hdr):
    global WHOLE, EXITS
    WHOLE = True
    EXITS = False
    ns = WHOLE_NS
    LINK.clear()
    funcs = {}
    known = set(WHOLE_ROOTS)
    changed = True
    while changed:
        changed = False
        for e in sorted(known):
            if e in funcs:
                continue
            funcs[e] = discover(e, known, frozenset(), WHOLE_MULTI.get(e))
            for c in funcs[e].calls | funcs[e].tails:
                if c not in known:
                    known.add(c)
                    changed = True
    base = os.path.basename(hdr)
    out = ['#include "%s"' % base, "", "namespace %s {" % ns, "", "#define FAIL(pc) do { ctl.fail = true; ctl.out = true; ctl.fail_pc = pc; return; } while (0)", ""]
    inline_max = int(os.environ.get("RJ_INLINE_MAX", "120"))
    for e in sorted(funcs):
        if e not in WHOLE_ROOTS and len(funcs[e].insns) <= inline_max and not (funcs[e].calls & set(funcs)) - set(INLINE_EXCEPT):
            SPEC[e] = "static inline __attribute__((always_inline)) "
    out.append("void icall(V* R, uint8_t* ls, Ctl& ctl, uint32_t t, uint32_t pc);")
    for e in sorted(funcs):
        out.append("%svoid f_%05x(V* R, uint8_t* ls, Ctl& ctl%s);" % (SPEC.get(e, ""), e, ", uint32_t entry" if e in WHOLE_MULTI else ""))
    out.append("")
    closure(funcs)
    for e in sorted(funcs):
        emit_func(funcs[e], out)
    cases = " ".join("case 0x%05x: f_%05x(R, ls, ctl%s); return;" % (e, e, ", 0x%05x" % e if e in WHOLE_MULTI else "") for e in sorted(funcs) if e not in SPEC and e not in PM_CALLS)
    cases += " " + " ".join("case 0x%05x: %s return;" % (t, c) for t, c in sorted(PM_CALLS.items()))
    if KERNEL_IMG:
        kp = sorted({(pc, d["w"]) for f in funcs.values() for pc, d in f.insns.items() if pc < PM_LS})
        out.append("static const uint32_t kKern[%d][2] = {%s};" % (max(len(kp), 1), ", ".join("{0x%05x, 0x%08xu}" % x for x in kp) or "{0, 0}"))
        out.append("static bool kernel_ok(const uint8_t* ls) { thread_local const uint8_t* ok = nullptr; if (ok == ls) return true; for (const auto& k : kKern) { uint32_t w; std::memcpy(&w, ls + k[0], 4); if (__builtin_bswap32(w) != k[1]) return false; } ok = ls; return true; }")
    out.append("void icall(V* R, uint8_t* ls, Ctl& ctl, uint32_t t, uint32_t pc) { switch (t & 0x3fffc) { %s default: %s } }" % (cases, WHOLE_ICALL_DEFAULT))
    out = [re.sub(r"(?<![A-Za-z0-9_])st\(ls,", "stp(ls,", l) for l in out]
    out = [re.sub(r"(?<![A-Za-z0-9_])st32\(ls,", "st32p(ls,", l) for l in out]
    out = [abi(l) for l in out]
    out += ["#undef FAIL", "", "}"]
    open(outp, "w", newline="\n").write("\n".join(out) + "\n")
    h = ["#pragma once", "", '#include "%s"' % OPS_INCLUDE] + list(WHOLE_HDR_EXTRA) + ["", "namespace %s {" % ns, "", "using namespace %s;" % BASE_NS, ""]
    if WHOLE_PM_DECLS:
        h.append("void pm_gettag(V* R, uint8_t* ls, Ctl& ctl);")
        h.append("void pm_releasetag(V* R, uint8_t* ls, Ctl& ctl);")
    for e in WHOLE_ROOTS:
        h.append("void f_%05x(V* R, uint8_t* ls, Ctl& ctl%s);" % (e, ", uint32_t entry" if e in WHOLE_MULTI else ""))
    h.append("inline constexpr uint32_t kWholeFuncs = %d;" % len(funcs))
    h += ["", "}"]
    h = [abi(l) for l in h]
    open(hdr, "w", newline="\n").write("\n".join(h) + "\n")
    print("whole: %d functions, %d insns, %d with fail" % (len(funcs), sum(len(f.insns) for f in funcs.values()), sum(1 for f in funcs.values() if f.has_fail)))


def main():
    if sys.argv[1] == "whole":
        whole_main(sys.argv[2], sys.argv[3])
        return
    if sys.argv[1] == "ops":
        ops_mode(sys.argv[2], sys.argv[3])
        return
    if EXITS and os.environ.get("RJ_NOCALL", "1") == "1":
        while True:
            found = generate(dry=True)
            if not (found - NOCALL):
                break
            NOCALL.update(found)
    generate(dry=False)


def generate(dry):
    TU.clear()
    TD.clear()
    SPEC.clear()
    if sys.argv[1] == "ops":
        ops_mode(sys.argv[2], sys.argv[3])
        return
    froots = []
    regions = {}
    spec = sys.argv[1]
    if spec.startswith("@"):
        spec = open(spec[1:]).read().strip()
    for tok in spec.split(","):
        if not tok:
            continue
        if ":" in tok:
            a, b = tok.split(":")
            regions[int(a, 16)] = frozenset(int(x, 16) for x in b.split("/") if x)
        else:
            froots.append(int(tok, 16))
    outp = sys.argv[2]
    hdr = sys.argv[3]
    ns = sys.argv[4] if len(sys.argv) > 4 else BASE_NS
    table = len(sys.argv) > 5
    global BUDGET
    BUDGET = table
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
    out = ['#include "%s"' % base] + (['#include "rj_prof.h"'] if PROF else []) + ["", "namespace %s {" % ns, "", "#define FAIL(pc) do { ctl.fail = true; ctl.out = true; ctl.fail_pc = pc; return; } while (0)", "#define EXIT(pc) do { R[0] = pref_only(pc); ctl.out = true; return; } while (0)", ""]
    inline_max = int(os.environ.get("RJ_INLINE_MAX", "120"))
    for e in sorted(funcs):
        if e not in froots and not table and len(funcs[e].insns) <= inline_max and not (funcs[e].calls & set(funcs)) - set(INLINE_EXCEPT):
            SPEC[e] = "static inline __attribute__((always_inline)) "
    for e in sorted(funcs):
        out.append("%svoid f_%05x(V* R, uint8_t* ls, Ctl& ctl);" % (SPEC.get(e, ""), e))
    out.append("")
    closure(funcs)
    REC_EDGES.clear()
    if RECURSION_LIMIT:
        REC_EDGES.update(rec_edges(funcs))
        out.append("inline thread_local uint32_t rj_depth = 0;")
        out.append("")
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
    if dry:
        return {e for e, fn in funcs.items() if fn.has_fail}
    if not table:
        out = [re.sub(r"(?<![A-Za-z0-9_])st\(ls,", "stp(ls,", l) for l in out]
        out = [re.sub(r"(?<![A-Za-z0-9_])st32\(ls,", "st32p(ls,", l) for l in out]
    out = [abi(l) for l in out]
    out.append("#undef FAIL")
    out.append("#undef EXIT")
    out.append("")
    out.append("}")
    open(outp, "w", newline="\n").write("\n".join(out) + "\n")
    h = ["#pragma once", "", '#include "%s"' % OPS_INCLUDE, "", "namespace %s {" % ns, "", "using namespace %s;" % BASE_NS, ""]
    for e in sorted(froots if not table else funcs):
        h.append("void f_%05x(V* R, uint8_t* ls, Ctl& ctl);" % e)
    for e in sorted(regs):
        h.append("void g_%05x(V* R, uint8_t* ls, Ctl& ctl);" % e)
    h.append("")
    allroots = sorted(set(froots) | set(regs))
    for e in allroots:
        h.append("inline constexpr uint32_t kSig%05x[16] = {%s};" % (e, ", ".join("0x%08xu" % word(e + 4 * i) for i in range(16))))
    h.append("")
    def closure_fails(g):
        seen = set()
        work = [g]
        while work:
            x = work.pop()
            if id(x) in seen:
                continue
            seen.add(id(x))
            if x.has_fail:
                return True
            for c in x.calls | x.tails:
                if c in funcs:
                    work.append(funcs[c])
        return False

    def root_fails(e):
        return closure_fails(regs[e] if e in regs else funcs[e])
    h.append("#define %s(X) " % ROOTS_MACRO + " ".join("X(%05x, %s_%05x, %d)" % (e, "g" if e in regs else "f", e, 1 if root_fails(e) else 0) for e in allroots))
    if table:
        h.append("")
        h.append("struct GenEntry { uint32_t lsa; void (RJ_ABI *fn)(V*, uint8_t*, Ctl&); };")
        h.append("inline constexpr GenEntry kAllGen[] = {%s};" % ", ".join("{0x%05x, f_%05x}" % (e, e) for e in sorted(funcs)))
    h += ["", "}"]
    h = [abi(l) for l in h]
    open(hdr, "w", newline="\n").write("\n".join(h) + "\n")
    for e in sorted(funcs):
        print("%05x insns %d calls %s tails %s" % (e, len(funcs[e].insns), sorted("%05x" % c for c in funcs[e].calls), sorted("%05x" % c for c in funcs[e].tails)))
    for e in sorted(regs):
        print("region %05x insns %d exits %s family %s journal %d" % (e, len(regs[e].insns), sorted("%05x" % c for c in regs[e].exits), "%05x" % regs[e].entry, 1 if root_fails(e) else 0))


if __name__ == "__main__":
    main()
