import glob
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "pm"))
import pm_cgen as G

NATIVES = frozenset({0x088B0, 0x0DBC8, 0x0E550, 0x0F538, 0x0BBC8, 0x0DCB0, 0x0D618, 0x0A880, 0x073E0, 0x0D158})

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def recorded_starts(img_ls, img):
    out = set()
    for path in glob.glob(os.path.join(os.environ.get("GOWA_SPU_BLOCKS") or os.path.join(ROOT, "recomp", "spu", "blocks"), "run*.bin")):
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


elf = open((os.environ.get("GOWA_ELF") or os.path.join(ROOT, "bin", "01.12", "GOWA.patched.elf")), "rb").read()
IMAGE = elf[0x737580 - 0x10000: 0x737580 - 0x10000 + 0xC430]

G.configure(
    PM_EA=0x737580,
    PM_LS=0x4000,
    PM_SIZE=0xC430,
    CODE=((0x4030, 0x10430),),
    DATA=None,
    API_TABLE=None,
    ENTRIES=(0x0AD98,),
    TRAMPOLINE=None,
    EXIT_TARGETS=NATIVES,
    BLOCK_BUDGET=True,
    IRQ_SLICE=65536,
    NS="pm::gen::ej",
    OPS_H="../pm/pm_ops.h",
    HEADER_BYTES=0x30,
    EXTRA_ROOTS=tuple(sorted(recorded_starts(0x4000, IMAGE) - NATIVES)),
    DISPATCH_TABLE=True,
)

if __name__ == "__main__":
    G.emit(sys.argv[1], sys.argv[2])
