import re
import sys
from collections import defaultdict


def load(path):
    fns = {}
    edges = defaultdict(dict)
    totals = {}
    for line in open(path):
        m = re.match(r"image (\S+) insns (\d+)", line)
        if m:
            totals[m.group(1)] = int(m.group(2))
        m = re.match(r"fn (\S+) +(\w+) calls +(\d+) incl +(\d+) excl +(\d+) per-call +\S+ impure +(\d+)", line)
        if m and m.group(1) != "-":
            fns[(m.group(1), int(m.group(2), 16))] = dict(calls=int(m.group(3)), incl=int(m.group(4)), excl=int(m.group(5)), impure=int(m.group(6)))
        m = re.match(r"edge (\S+) (\w+) -> (\w+) calls (\d+) incl (\d+)", line)
        if m:
            edges[(m.group(1), int(m.group(2), 16))][int(m.group(3), 16)] = (int(m.group(4)), int(m.group(5)))
    return fns, edges, totals


def main():
    fns, edges, totals = load(sys.argv[1])
    minper = float(sys.argv[2]) if len(sys.argv) > 2 else 200
    for img in sorted(totals):
        roots = defaultdict(int)
        for (im, f), d in fns.items():
            if im != img or d["impure"] == 0:
                continue
            for c, (n, incl) in edges.get((im, f), {}).items():
                cd = fns.get((im, c))
                if cd and cd["impure"] == 0:
                    roots[c] += incl
        tot = totals[img]
        s = 0
        print("%s total %d" % (img, tot))
        for c, incl in sorted(roots.items(), key=lambda x: -x[1]):
            d = fns[(img, c)]
            per = d["incl"] / max(1, d["calls"])
            if per < minper or incl < tot * 0.003:
                continue
            s += incl
            print("  pure root %05x (LS %05x) incl-from-impure %10d (%.1f%%) per-call %.0f cum %.1f%%" % (c, c + 0x4000, incl, 100.0 * incl / tot, per, 100.0 * s / tot))
        imp = sorted(((d["excl"], f) for (im, f), d in fns.items() if im == img and d["impure"]), reverse=True)
        print("  impure exclusive:", " ".join("%05x:%.1f%%" % (f, 100.0 * e / tot) for e, f in imp[:20]))


main()
