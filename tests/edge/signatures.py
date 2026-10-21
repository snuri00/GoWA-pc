import struct
import sys

ELF = "bin/01.12/GOWA.patched.elf"
IMAGE_EA = 0x737580
LS_BASE = 0x4000


def words(d, ls, n):
    off = IMAGE_EA - 0x10000 + (ls - LS_BASE)
    return struct.unpack_from(">%dI" % n, d, off)


if __name__ == "__main__":
    d = open(ELF, "rb").read()
    n = 16
    for a in sys.argv[1:]:
        ls = int(a, 16)
        w = words(d, ls, n)
        print("const uint32_t kSig%05x[] = {%s};" % (ls, ", ".join("0x%08xu" % x for x in w)))
