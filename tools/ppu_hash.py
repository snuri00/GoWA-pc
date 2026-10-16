import glob
import hashlib
import os
import struct
import sys


def ppu_hash(path):
    d = open(path, "rb").read()
    e_phoff = struct.unpack(">Q", d[0x20:0x28])[0]
    e_phentsize, e_phnum = struct.unpack(">HH", d[0x36:0x3A])
    sha = hashlib.sha1()
    for i in range(e_phnum):
        ph = d[e_phoff + i * e_phentsize:e_phoff + (i + 1) * e_phentsize]
        p_type, p_flags, p_offset, p_vaddr, _, p_filesz, p_memsz = struct.unpack(">IIQQQQQ", ph[:0x30])
        sha.update(ph[0:4])
        sha.update(ph[4:8])
        if p_type == 1 and p_memsz:
            sha.update(ph[0x10:0x18])
            sha.update(ph[0x28:0x30])
            sha.update(d[p_offset:p_offset + p_filesz])
    return "PPU-" + sha.hexdigest()


if __name__ == "__main__":
    for p in sorted(glob.glob(os.path.join(sys.argv[1], "*", "*.elf"))):
        print(ppu_hash(p), os.path.relpath(p, sys.argv[1]))
