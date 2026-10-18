import struct
import sys

ELF = sys.argv[1] if len(sys.argv) > 1 else "bin/01.12/GOWA.patched.elf"
BASE = 0x754E00


def fnv1(s):
    h = 0x811C9DC5
    for c in s.encode():
        h = ((h * 0x01000193) & 0xFFFFFFFF) ^ c
    return h


def table(d):
    off = BASE - 0x10000
    cnt, doff = struct.unpack_from(">HH", d, off)
    names = {}
    for p, n in (("smDecompressVertices_", 32), ("smCompressVertices_", 32), ("smSetVertexDataArrays_", 32)):
        for i in range(n):
            names[fnv1(p + str(i))] = p + str(i)
    out = []
    for i in range(cnt):
        h, q, sz = struct.unpack_from(">IHH", d, off + 4 + 8 * i)
        out.append((names.get(h, "hash_%08x" % h), BASE + doff + 16 * q, sz))
    return out


if __name__ == "__main__":
    d = open(ELF, "rb").read()
    for name, ea, sz in sorted(table(d), key=lambda t: t[1]):
        print("%-26s ea %06x size %4x" % (name, ea, sz))
