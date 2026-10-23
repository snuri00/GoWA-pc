import glob
import os
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ELF = open((os.environ.get("GOWA_ELF") or os.path.join(ROOT, "bin", "01.12", "GOWA.patched.elf")), "rb").read()
MODS = {0x7e1c00: "mp3pack", 0x7aae00: "synth_submix", 0x7bfc80: "aux_master", 0x7ff280: "mp3_sampler_surround", 0x705680: "renjob"}
HDR = {ELF[ea - 0x10000:ea - 0x10000 + 16]: n for ea, n in MODS.items()}


def scan(path):
    raw = open(path, "rb").read()
    p = 4 * 7 + 2048
    ls0 = raw[p:p + 0x40000]
    p += 0x40000
    count = struct.unpack_from("<I", raw, p)[0]
    p += 4
    found = {}
    for lsa in range(0, 0x40000, 128):
        n = HDR.get(ls0[lsa:lsa + 16])
        if n:
            found.setdefault(n, []).append(("init", lsa))
    for i in range(count):
        kind, ch, val, pc, srr0 = struct.unpack_from("<iIIII", raw, p)
        p += 24
        lines = struct.unpack_from("<I", raw, p)[0]
        p += 4
        for k in range(lines):
            l = struct.unpack_from("<I", raw, p)[0]
            n = HDR.get(raw[p + 4:p + 20])
            if n:
                found.setdefault(n, []).append((i, l))
            p += 132
    return found


for pat in sys.argv[1:]:
    for f in sorted(glob.glob(pat)):
        r = scan(f)
        print(f, {k: (len(v), sorted(set(x[1] for x in v))[:4]) for k, v in r.items()})
