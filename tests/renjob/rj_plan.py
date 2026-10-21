import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rj_cgen

cg = vars(rj_cgen)
_argv = sys.argv


def load_impure(path, ratio):
    imp = set()
    for line in open(path):
        m = re.match(r"fn (\w+) calls +(\d+) incl +(\d+).*impure +(\d+)", line)
        if m and int(m.group(4)) > ratio * int(m.group(2)):
            imp.add(int(m.group(1), 16))
    return imp


def walk(fn):
    decode = cg["decode"]
    seen = {}
    work = [fn]
    while work:
        pc = work.pop()
        while pc not in seen:
            d = decode(pc)
            seen[pc] = d
            op = d["op"]
            if op == "br":
                work.append((pc + d["imm"] * 4) & 0x3FFFC)
                break
            if op in ("brz", "brnz", "brhz", "brhnz"):
                work.append((pc + d["imm"] * 4) & 0x3FFFC)
            if op == "bi":
                if d["ra"] != 0 and d["rb"] & 0x30:
                    pc += 4
                    continue
                if d["ra"] != 0 and cg["jump_table"](pc):
                    work.extend(cg["jump_table"](pc))
                break
            if op in ("stop", "stopd", "invalid", "bra", "brasl", "iret"):
                break
            pc += 4
    return seen


EXITING = {}


def exiting(fn, depth=0):
    if fn in EXITING:
        return EXITING[fn]
    EXITING[fn] = False
    r = False
    for pc, d in walk(fn).items():
        op = d["op"]
        if op in ("rdch", "wrch", "rchcnt", "stop", "stopd", "bisl"):
            r = True
        elif op == "bi" and d["ra"] != 0 and d["rb"] & 0x30:
            r = True
        elif op == "brsl" and d["rt"] == 0:
            t = (pc + d["imm"] * 4) & 0x3FFFC
            if t != pc + 4 and depth < 12 and exiting(t, depth + 1):
                r = True
    EXITING[fn] = r
    return r


def plan(fn, impure):
    insns = walk(fn)
    stops = set()
    starts = {fn}
    nostop = os.environ.get("RJ_PLAN_NOSTOP") == "1"
    for pc, d in sorted(insns.items()):
        op = d["op"]
        if op == "brsl" and d["rt"] == 0:
            t = (pc + d["imm"] * 4) & 0x3FFFC
            if nostop:
                if t != pc + 4 and t in impure:
                    starts.add(pc + 4)
            elif t in impure:
                stops.add(pc)
                starts.add(pc + 4)
        elif op in ("rdch", "wrch", "rchcnt", "stop", "stopd", "bisl"):
            stops.add(pc)
        elif op == "bi" and d["ra"] != 0 and d["rb"] & 0x30:
            stops.add(pc)
            if d["rb"] & 0x10:
                starts.add(pc + 4)
    targets = set()
    for pc, d in insns.items():
        op = d["op"]
        if op in ("br", "brz", "brnz", "brhz", "brhnz"):
            targets.add((pc + d["imm"] * 4) & 0x3FFFC)
        if op in ("brsl", "bisl"):
            targets.add(pc + 4)
        if op == "bi" and d["ra"] != 0 and cg["jump_table"](pc):
            targets.update(cg["jump_table"](pc))
        if op == "bi" and d["ra"] != 0 and d["rb"] & 0x30:
            targets.add(pc + 4)
    enders = ("br", "brz", "brnz", "brhz", "brhnz", "brsl", "bi", "bisl", "biz", "binz", "bihz", "bihnz", "stop", "stopd", "rdch", "wrch", "rchcnt")
    extra = set()
    for x in stops:
        b = x
        while b not in targets and (b - 4) in insns and insns[b - 4]["op"] not in enders:
            b -= 4
        if b != x and cg["r0_dead"](b):
            extra.add(b)
    if os.environ.get("RJ_PLAN_NOEXTRA") != "1":
        stops |= extra
    return sorted(starts), sorted(stops)


def main():
    calls = _argv[1]
    ratio = float(_argv[2])
    impure = load_impure(calls, ratio)
    specs = []
    for tok in _argv[3].split(","):
        fn = int(tok, 16)
        starts, stops = plan(fn, impure)
        for s in starts:
            specs.append("%05x:%s" % (s, "/".join("%05x" % x for x in stops)))
    print(",".join(specs))


main()
