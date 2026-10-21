import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import spu_dis as D
from sc_cgen import IMAGES

ELF = open(os.path.join(ROOT, "bin", "01.12", "GOWA.patched.elf"), "rb").read()


def main():
    c = IMAGES[sys.argv[1]]
    lo = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x4030
    hi = int(sys.argv[3], 16) if len(sys.argv) > 3 else c["code_end"]
    counts = {}
    if len(sys.argv) > 4:
        for line in open(sys.argv[4]):
            a, b = line.split()
            counts[int(a, 16)] = int(b)
    img = ELF[c["ea"] - 0x10000:c["ea"] - 0x10000 + c["size"]]
    for pc in range(lo, hi, 4):
        w = struct.unpack_from(">I", img, pc - 0x4000)[0]
        n = counts.get(pc)
        print("%05x: %08x  %-32s%s" % (pc, w, D.dis(w, pc), " %d" % n if n else ""))


if __name__ == "__main__":
    main()
