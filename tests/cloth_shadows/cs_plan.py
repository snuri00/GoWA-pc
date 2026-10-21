import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cs_cgen as G


def load_calls(path):
    fns = {}
    for line in open(path):
        m = re.match(r"fn (\w+) calls +(\d+) incl +(\d+) .*excl +(\d+) .*impure +(\d+) callers(.*)", line)
        if m:
            fns[int(m.group(1), 16)] = dict(calls=int(m.group(2)), incl=int(m.group(3)), excl=int(m.group(4)), impure=int(m.group(5)), callers=[int(x, 16) for x in m.group(6).split()])
    return fns


def body(fn):
    f = G.discover(fn, set())
    return f


def impure_sites(f, impure):
    sites = []
    for pc, d in sorted(f.insns.items()):
        op = d["op"]
        if op == "brsl" and d["rt"] == 0:
            t = (pc + d["imm"] * 4) & 0x3FFFC
            if t in impure and t != pc + 4:
                sites.append(pc)
        elif op in ("rdch", "wrch", "rchcnt", "stop", "stopd", "bisl"):
            sites.append(pc)
        elif op == "bi" and d["ra"] != 0 and d["rb"] & 0x30:
            sites.append(pc)
    return sites


def main():
    G.select(sys.argv[1])
    fns = load_calls(sys.argv[2])
    min_insns = float(sys.argv[3]) if len(sys.argv) > 3 else 40
    impure = {e for e, s in fns.items() if s["impure"] > 0}
    owner = {}
    bodies = {}
    for e in fns:
        if not (G.IMG_LS <= e < G.JOB["code_end"]):
            continue
        bodies[e] = body(e)
        for pc in bodies[e].insns:
            owner.setdefault(pc, set()).add(e)
    roots = []
    for e, s in sorted(fns.items()):
        if e in impure or e not in bodies:
            continue
        from_impure = any(owner.get(c, set()) & impure or not owner.get(c) for c in s["callers"])
        if from_impure and s["incl"] / max(1, s["calls"]) >= min_insns:
            roots.append(e)
    families = []
    for e in sorted(impure):
        if e not in bodies:
            continue
        f = bodies[e]
        starts = {e}
        for pc in impure_sites(f, impure):
            d = f.insns[pc]
            if d["op"] == "brsl" or (d["op"] == "bi" and d["rb"] & 0x10):
                starts.add(pc + 4)
        families.append((e, sorted(starts)))
    for e in roots:
        s = fns[e]
        print("root %05x calls %d incl %d per-call %.0f" % (e, s["calls"], s["incl"], s["incl"] / max(1, s["calls"])), file=sys.stderr)
    for e, st in families:
        s = fns[e]
        print("family %05x excl %d starts %s" % (e, s["excl"], " ".join("%05x" % x for x in st)), file=sys.stderr)
    print(",".join("%05x" % r for r in roots))


main()
