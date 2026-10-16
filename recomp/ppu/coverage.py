import collections
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from inventory import Elf
from isa import mnemonic


def text_range(elf):
    best = None
    for s in elf.shdrs:
        if s[2] & 4 and s[5] and (best is None or s[5] > best[1]):
            best = (s[3], s[5])
    return best


def main(elf_path):
    elf = Elf(elf_path)
    lo, size = text_range(elf)
    blob = elf.read(lo, size)
    hist = collections.Counter()
    unknown = collections.Counter()
    unknown_sites = collections.defaultdict(list)
    for i in range(0, len(blob) - 3, 4):
        w = struct.unpack_from(">I", blob, i)[0]
        m = mnemonic(w)
        if m:
            hist[m] += 1
        else:
            key = f"op{w >> 26}/x{(w >> 1) & 0x3FF}"
            unknown[key] += 1
            unknown_sites[key].append((lo + i, w))
    total = len(blob) // 4
    print(f".text {lo:#x}+{size:#x}: {total} words, {len(hist)} mnemonics, {sum(unknown.values())} unknown")
    for k, v in unknown.most_common(40):
        print(f"  {k:14} {v:>7}  e.g. " + " ".join(f"{a:#x}:{w:08x}" for a, w in unknown_sites[k][:4]))
    print("top:", ", ".join(f"{k} {v}" for k, v in hist.most_common(30)))
    return hist, unknown


if __name__ == "__main__":
    main(sys.argv[1])
