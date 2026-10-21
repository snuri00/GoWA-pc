import glob
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tests", "pm"))
import pm_cgen as G
import rj_cgen as RJ

OUT = os.path.join(ROOT, "runtime", "src", "spu", "scream")

IMAGES = (
    ("mp3", 0x7E1C00, 0x1C0A0, 0x1B6D0),
    ("aux", 0x7BFC80, 0x20BC0, 0x204D0),
)


def codegen_extra_starts():
    text = open(os.path.join(ROOT, "recomp", "spu", "codegen.py")).read()
    out = set()
    for m in re.finditer(r"EXTRA_STARTS \|?= \{([^}]*)\}", text):
        out |= set(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]+)", m.group(1)))
    return out


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


RJ.RR3.update({"dfa", "dfs", "dfm"})
RJ.RR2.update({"fesd", "frds"})

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


def kernel_hooks(name):
    text = open(os.path.join(OUT, "%s_gen.h" % name)).read()
    m = re.search(r"#define SC_%s_ROOTS\(X\)(.*)" % name.upper(), text)
    return frozenset(int(a, 16) for a in re.findall(r"X\(([0-9a-f]+),", m.group(1)))


def generate(name, ea, size, code_end):
    elf = open(os.path.join(ROOT, "bin", "01.12", "GOWA.patched.elf"), "rb").read()
    img = elf[ea - 0x10000: ea - 0x10000 + size]
    entry = 0x4000 + struct.unpack_from(">I", img, 0x10)[0]
    exits = kernel_hooks(name)
    G.configure(
        PM_EA=ea,
        PM_LS=0x4000,
        PM_SIZE=size,
        CODE=((0x4030, code_end),),
        DATA=None,
        API_TABLE=None,
        ENTRIES=(entry,),
        TRAMPOLINE=None,
        EXIT_TARGETS=exits,
        BLOCK_BUDGET=True,
        IRQ_SLICE=65536,
        NS="pm::gen::scj_%s" % name,
        OPS_H="scream_job_ops.h",
        HEADER_BYTES=0x30,
        IE_INLINE=True,
        EXTRA_ROOTS=tuple(sorted(((recorded_starts(0x4000, img) | set(a for a in codegen_extra_starts() if 0x4030 <= a < code_end)) - exits))),
    )
    print(name, file=sys.stderr)
    cpp = os.path.join(OUT, "scj_%s_gen.cpp" % name)
    G.emit(cpp, os.path.join(OUT, "scj_%s_gen.h" % name))
    direct_registers(cpp)


def direct_registers(path):
    lines = open(path).read().split("\n")
    out = []
    for line in lines:
        if line.startswith("    V& r0 = R[0];"):
            continue
        out.append(re.sub(r"\br([0-9]+)\b", r"R[\1]", line))
    open(path, "w", newline="\n").write("\n".join(out))


if __name__ == "__main__":
    want = set(sys.argv[1:])
    for name, ea, size, code_end in IMAGES:
        if not want or name in want:
            generate(name, ea, size, code_end)
