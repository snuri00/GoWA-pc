import argparse
import glob
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KNOWN = {0x737580: "Edge", 0x730800: "TexCreator", 0x728400: "Cloth", 0x79c400: "Shadows", 0x705680: "RenJob",
         0x74b100: "MeshPrep", 0x6f8800: "PrimBuilder", 0x793500: "GroupVis", 0x6f4a00: "SortPrim",
         0x78b380: "MeshHier", 0x84be40: "Anim", 0x7e1c00: "ScreamMP3", 0x7bfc80: "ScreamAux"}
OVERLAYS = {0x6eee30: "AnimOvl", 0x6f1730: "AnimOvl", 0x718c30: "AnimOvl", 0x7529b0: "AnimOvl"}


def module_headers(elf):
    d = open(elf, "rb").read()
    out = []
    pos = 0
    while True:
        i = d.find(b"\xc0\xde\xc0\xde", pos)
        if i < 0:
            return sorted(out)
        pos = i + 4
        if (i - 0x20) % 16 == 0:
            out.append(i - 0x20 + 0x10000)


def namer(elf):
    heads = module_headers(elf)

    def name(ea):
        if ea in OVERLAYS:
            return OVERLAYS[ea]
        base = max((h for h in heads if h <= ea), default=None)
        if base is None or ea - base > 0x30000:
            return "ea_%06x" % ea
        n = KNOWN.get(base, "img_%06x" % base)
        return "EdgeOvl" if n == "Edge" and ea >= base + 0xc430 else n
    return name


def parse(path, name):
    acc = {}
    lines = [l for l in open(path, encoding="utf-8", errors="replace") if l.startswith("avg SPUs per job")]
    for l in lines:
        for m in re.finditer(r"\[(kernel|hostwait\d|wid \d+ ([0-9a-f]{8})) ([0-9.]+)\]", l):
            k = name(int(m.group(2), 16)) if m.group(2) else m.group(1)
            acc[k] = acc.get(k, 0) + float(m.group(3)) / len(lines)
    return acc


def main():
    ap = argparse.ArgumentParser(description="SPUs per job in the low-fps windows captured by GOWA_SPU_CAPTURE_LOWFPS")
    ap.add_argument("dir", help="session dir (out/play/<stamp>) or its spucap dir")
    ap.add_argument("--elf", default=os.environ.get("GOWA_ELF") or os.path.join(ROOT, "bin", "01.12", "GOWA.patched.elf"))
    args = ap.parse_args()
    occ = sorted(glob.glob(os.path.join(args.dir, "**", "occ.txt"), recursive=True))
    if not occ:
        print("no occ.txt under", args.dir)
        return
    name = namer(args.elf)
    cols = [parse(p, name) for p in occ]
    keys = sorted(set().union(*cols), key=lambda k: -sum(c.get(k, 0) for c in cols))
    print("%-12s" % "" + "".join("%14s" % os.path.basename(os.path.dirname(p))[:13] for p in occ))
    for k in keys:
        print("%-12s" % k + "".join("%14.2f" % c.get(k, 0) for c in cols))


if __name__ == "__main__":
    main()
