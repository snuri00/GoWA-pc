import glob
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "pm"))
import pm_cgen as G

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join((os.environ.get("GOWA_GEN_DIR") or os.path.join(ROOT, "out", "gen", "spu")), "tasks")

TASKS = (
    ("zlib", 0x6E5900),
    ("particle", 0x6DB580),
    ("t6c3e00", 0x6C3E00),
)


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
            if off < 0 or off + 4 * n > len(img):
                continue
            if all(struct.pack("<I", r) == img[off + 4 * k: off + 4 * k + 4] for k, r in enumerate(raw)):
                out.add(lsa)
    return out


def text_segment(elf, ea):
    off = ea - 0x10000
    entry = struct.unpack_from(">I", elf, off + 0x18)[0]
    phoff = struct.unpack_from(">I", elf, off + 0x1C)[0]
    phn = struct.unpack_from(">H", elf, off + 0x2C)[0]
    for k in range(phn):
        p_type, p_off, p_vaddr, _, p_filesz, _, p_flags, _ = struct.unpack_from(">8I", elf, off + phoff + 32 * k)
        if p_type == 1 and p_flags & 1:
            return entry, ea + p_off, p_vaddr, p_filesz
    raise SystemExit("no text segment")


_discover = G.discover
_loop_heads = G.loop_heads
_labels = set()


def discover_all():
    insns, labels, taken = _discover()
    _labels.clear()
    _labels.update(labels)
    return insns, labels, taken


def every_leader(insns):
    return _loop_heads(insns) | G.leaders_of(insns, _labels)


G.discover = discover_all
G.loop_heads = every_leader


def generate(name, ea):
    elf = open((os.environ.get("GOWA_ELF") or os.path.join(ROOT, "bin", "01.12", "GOWA.patched.elf")), "rb").read()
    entry, text_ea, ls, size = text_segment(elf, ea)
    img = elf[text_ea - 0x10000: text_ea - 0x10000 + size]
    G.configure(
        PM_EA=text_ea,
        PM_LS=ls,
        PM_SIZE=size,
        CODE=((ls, ls + size),),
        DATA=None,
        API_TABLE=None,
        ENTRIES=(entry,),
        TRAMPOLINE=None,
        EXIT_TARGETS=frozenset(),
        BLOCK_BUDGET=True,
        IRQ_SLICE=65536,
        NS="pm::gen::tk_%s" % name,
        OPS_H="../pm/pm_ops.h",
        HEADER_BYTES=0x30,
        EXTRA_ROOTS=tuple(sorted(recorded_starts(ls, img))),
        DISPATCH_TABLE=True,
        DISPATCH_GATE="spuf::glue_disp_on()",
    )
    print(name, file=sys.stderr)
    G.emit(os.path.join(OUT, "tk_%s_gen.cpp" % name), os.path.join(OUT, "tk_%s_gen.h" % name))


if __name__ == "__main__":
    want = set(sys.argv[1:])
    for name, ea in TASKS:
        if not want or name in want:
            generate(name, ea)
