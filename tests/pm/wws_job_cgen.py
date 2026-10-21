import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pm_cgen as G

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def recorded_starts(img_ls, img):
    out = set()
    for path in glob.glob(os.path.join(ROOT, "recomp", "spu", "blocks", "run*.bin")):
        data = open(path, "rb").read()
        words = struct.unpack("<%dI" % (len(data) // 4), data[: len(data) // 4 * 4])
        i = 0
        while i + 2 <= len(words):
            lsa, n = words[i], words[i + 1]
            raw = words[i + 2: i + 2 + n]
            if len(raw) < n:
                break
            i += 2 + n
            off = lsa - img_ls
            if off < 0x30 or off + 4 * n > len(img):
                continue
            if all(struct.pack("<I", r) == img[off + 4 * k: off + 4 * k + 4] for k, r in enumerate(raw)):
                out.add(lsa)
    return out


def generate(ea, size, ns, out_cpp, out_h, exits=frozenset(), ls=0x4000):
    elf = open(os.path.join(ROOT, "bin", "01.12", "GOWA.patched.elf"), "rb").read()
    img = elf[ea - 0x10000: ea - 0x10000 + size]
    entry = ls + struct.unpack_from(">I", img, 0x10)[0]
    G.configure(
        PM_EA=ea,
        PM_LS=ls,
        PM_SIZE=size,
        CODE=((ls + 0x30, ls + size),),
        DATA=None,
        API_TABLE=None,
        ENTRIES=(entry,),
        TRAMPOLINE=None,
        EXIT_TARGETS=frozenset(exits),
        BLOCK_BUDGET=True,
        IRQ_SLICE=65536,
        NS=ns,
        OPS_H="../pm/pm_ops.h",
        HEADER_BYTES=0x30,
        EXTRA_ROOTS=tuple(sorted(recorded_starts(ls, img) - set(exits))),
        DISPATCH_TABLE=True,
        DISPATCH_GATE="spuf::glue_disp_on()",
    )
    G.emit(out_cpp, out_h)
