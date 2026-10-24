import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tests", "renjob"))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import rj_cgen as G
import spu_dis as D

JOBS = (
    ("primbuilder", "pb", 0x6F8800, 0xCE30, "primbuilder", ()),
    ("groupvis", "gv", 0x793500, 0x7080, "groupvis", ()),
    ("sortprim", "sp", 0x6F4A00, 0x2790, "sortprim", (0x4080,)),
    ("meshprep", "mp", 0x74B100, 0x4440, "meshprep", ()),
    ("meshhier", "mh", 0x78B380, 0x8130, None, ()),
    ("shadows", "sh", 0x79C400, 0x9850, None, ()),
    ("anim", "an", 0x84BE40, 0xAA70, None, ()),
)

PM_API = (0x16F0, 0x2FD8, 0x1760, 0x17B8, 0x17C0, 0x17E0, 0x1AF0, 0x1B10, 0x1A90)


def u32(data, ea):
    return struct.unpack_from(">I", data, ea - 0x10000)[0]


def crt(data, ea):
    entry = 0x4000 + u32(data, ea + 0x10)
    tail = None
    tabs = []
    for i in range(160):
        pc = entry + 4 * i
        s = D.dis(u32(data, ea + pc - 0x4000), pc)
        if s.startswith("ila r0,") and tail is None:
            tail = int(s.split(",")[1], 16) + 0x4000
        if s.startswith("ila r80,"):
            tabs.append(int(s.split(",")[1], 16) + 0x4000)
    ctors = []
    for t in tabs:
        for j in range(64):
            v = u32(data, ea + t - 0x4000 + 4 * j)
            if v == 0xFFFFFFFF or v == 0:
                break
            ctors.append(v + 0x4000)
    return entry, tail, ctors


def icalls(name, lo, hi):
    out = set()
    if not name:
        return out
    path = os.path.join(ROOT, "tests", "meshprep", "icalls_%s.txt" % name)
    if os.path.exists(path):
        for line in open(path):
            for tok in line.split()[1:]:
                t = int(tok.split(":")[0], 16)
                if lo <= t < hi:
                    out.add(t)
    return out


def extra_icalls(short, lo, hi):
    out = set()
    path = os.path.join(HERE, "icalls_%s.txt" % short)
    if os.path.exists(path):
        for tok in open(path).read().split():
            t = int(tok, 16)
            if lo <= t < hi:
                out.add(t)
    return out


def generate(job, outdir):
    name, short, ea, size, ic, extra = job
    data = G.data
    entry, tail, ctors = crt(data, ea)
    lo, hi = 0x4030, 0x4000 + size
    found = (set(ctors) | icalls(ic, lo, hi) | extra_icalls(short, lo, hi) | set(extra)) - {entry}
    roots = [entry] + sorted(r for r in found if lo <= r < hi) + list(PM_API)
    pc_counts = os.path.join(ROOT, "tests", "meshprep", "pc_counts_%s.txt" % ic) if ic else None
    G.configure(IMG_EA=ea, IMG_SIZE=size, CODE_END=hi, ENTRY_ROOTS=(entry,), INLINE_EXCEPT=(), OPS_INCLUDE="renjob_ops.h", BASE_NS="renjob::xo",
                PC_COUNTS_PATH=pc_counts, KERNELS={}, ICALLS={}, RECURSION_LIMIT=0, LOOP_BUDGET=False,
                WHOLE_ROOTS=tuple(roots), WHOLE_MULTI={entry: (entry, tail)}, WHOLE_NS="renjob::wj_%s" % short, WHOLE_PM=True, WHOLE_RETCHECK=True,
                WHOLE_HDR_EXTRA=('#include "jobs_flags.h"',), WHOLE_PM_DECLS=False,
                PM_CALLS={0x017E0: "if (::wj::t_lnj) { f_017e0(R, ls, ctl); if (ctl.out) return; }"})
    G.PC_COUNTS = None
    G.NOCALL.clear()
    G.SPEC.clear()
    G.TU.clear()
    G.TD.clear()
    cpp = os.path.join(outdir, "wj_%s_gen.cpp" % short)
    h = os.path.join(outdir, "wj_%s_gen.h" % short)
    G.whole_main(cpp, h)
    hl = open(h).read().rstrip("\n").split("\n")
    assert hl[-1] == "}"
    add = ["inline constexpr uint32_t kEntry = 0x%05x;" % entry, "inline constexpr uint32_t kTail = 0x%05x;" % tail, "inline constexpr uint32_t kImgEa = 0x%x;" % ea,
           "inline constexpr uint32_t kImgSize = 0x%x;" % size,
           "inline constexpr uint32_t kSig[12] = {%s};" % ", ".join("0x%08xu" % u32(data, ea + 4 * i) for i in range(12)),
           "inline constexpr uint32_t kEntrySig[16] = {%s};" % ", ".join("0x%08xu" % u32(data, ea + entry - 0x4000 + 4 * i) for i in range(16)),
           "inline void root(V* R, uint8_t* ls, Ctl& ctl, uint32_t e) { f_%05x(R, ls, ctl, e); }" % entry]
    hl = hl[:-1] + add + ["", "}"]
    hl = [G.abi(l) for l in hl]
    open(h, "w", newline="\n").write("\n".join(hl) + "\n")


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ["GOWA_GEN_DIR"], "jwhole")
    want = set(sys.argv[2:])
    os.makedirs(outdir, exist_ok=True)
    for job in JOBS:
        if want and job[0] not in want and job[1] not in want:
            continue
        generate(job, outdir)
        print(job[0], file=sys.stderr)


if __name__ == "__main__":
    main()
