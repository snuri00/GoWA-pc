import glob
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
INTERP = os.path.join(ROOT, "runtime", "src", "spu", "interpreter.cpp")
BLOCKS_PER_FILE = 400

CONTROL = {
    "op_stop", "op_rdch", "op_wrch", "op_rchcnt", "op_illegal", "op_iret", "op_bisled",
    "op_br", "op_bra", "op_brsl", "op_brasl", "op_brz", "op_brnz", "op_brhz", "op_brhnz",
    "op_bi", "op_bisl", "op_biz", "op_binz", "op_bihz", "op_bihnz",
    "op_heq", "op_heqi", "op_hgt", "op_hgti", "op_hlgt", "op_hlgti", "op_mfspr",
}


BRANCHES = {
    "op_iret", "op_bisled", "op_br", "op_bra", "op_brsl", "op_brasl", "op_brz", "op_brnz", "op_brhz", "op_brhnz",
    "op_bi", "op_bisl", "op_biz", "op_binz", "op_bihz", "op_bihnz", "op_stop",
}


def decode_table():
    src = open(INTERP, encoding="utf-8").read()
    table = ["op_illegal"] * 2048
    for m in re.finditer(r"set\((0x[0-9A-Fa-f]+),\s*(\d+),\s*(op_\w+)\)", src):
        opcode, bits, name = int(m.group(1), 16), int(m.group(2)), m.group(3)
        shift = 11 - bits
        for k in range(1 << shift):
            table[(opcode << shift) | k] = name
    return table


def load_blocks(paths):
    blocks = {}
    for path in paths:
        data = open(path, "rb").read()
        words = struct.unpack("<%dI" % (len(data) // 4), data[: len(data) // 4 * 4])
        i = 0
        while i + 2 <= len(words):
            lsa, n = words[i], words[i + 1]
            raw = words[i + 2: i + 2 + n]
            if len(raw) < n:
                break
            blocks[(lsa, tuple(raw))] = True
            i += 2 + n
    return sorted(blocks.keys())


def is_pure(table, raw):
    for k, r in enumerate(raw):
        w = struct.unpack(">I", struct.pack("<I", r))[0]
        name = table[w >> 21]
        if name in CONTROL and not (k == len(raw) - 1 and name in BRANCHES and name not in ("op_bisled", "op_stop", "op_iret")):
            return False
    return True


def emit_block(table, idx, lsa, raw, out):
    out.append("bool b%d(Context& c, Host& h) {" % idx)
    out.append("    (void)h;")
    n = len(raw)
    last_branch = False
    for k, r in enumerate(raw):
        w = struct.unpack(">I", struct.pack("<I", r))[0]
        pc = (lsa + k * 4) & 0x3FFFC
        name = table[w >> 21]
        if name in CONTROL:
            out.append("    c.pc = 0x%x;" % ((pc + 4) & 0x3FFFC))
            out.append("    if (!%s(c, h, 0x%08xu, 0x%xu)) return false;" % (name, w, pc))
            if k != n - 1 and name not in BRANCHES:
                out.append("    if (c.pc != 0x%x) return true;" % ((pc + 4) & 0x3FFFC))
            last_branch = k == n - 1
        else:
            out.append("    %s(c, h, 0x%08xu, 0x%xu);" % (name, w, pc))
            last_branch = False
    if not last_branch:
        out.append("    c.pc = 0x%x;" % ((lsa + n * 4) & 0x3FFFC))
    out.append("    return true;")
    out.append("}")
    out.append("")


def main():
    if len(sys.argv) < 2:
        print("usage: codegen.py <out_dir> <blocks.bin>...")
        return 1
    out_dir = sys.argv[1]
    inputs = []
    for a in sys.argv[2:]:
        inputs.extend(glob.glob(a))
    os.makedirs(out_dir, exist_ok=True)
    for old in glob.glob(os.path.join(out_dir, "spu_*.cpp")):
        os.remove(old)
    table = decode_table()
    blocks = load_blocks(inputs)
    files = (len(blocks) + BLOCKS_PER_FILE - 1) // BLOCKS_PER_FILE
    for f in range(files):
        out = ['#include "ops_impl.h"', "", "namespace spu::gen {", ""]
        for i in range(f * BLOCKS_PER_FILE, min(len(blocks), (f + 1) * BLOCKS_PER_FILE)):
            lsa, raw = blocks[i]
            emit_block(table, i, lsa, raw, out)
        out.append("}")
        out.append("")
        open(os.path.join(out_dir, "spu_%03d.cpp" % f), "w", newline="\n").write("\n".join(out))
    reg = ['#include "spu/aot.h"', "", "namespace spu::gen {", ""]
    for i in range(len(blocks)):
        reg.append("bool b%d(Context& c, Host& h);" % i)
    reg.append("")
    for i, (lsa, raw) in enumerate(blocks):
        reg.append("const uint32_t w%d[] = {%s};" % (i, ", ".join("0x%08xu" % r for r in raw)))
    reg.append("")
    reg.append("}")
    reg.append("")
    reg.append("namespace spu {")
    reg.append("")
    if blocks:
        reg.append("const CompiledBlock g_blocks[] = {")
        for i, (lsa, raw) in enumerate(blocks):
            reg.append("    {0x%xu, %du, gen::w%d, gen::b%d, %s}," % (lsa, len(raw), i, i, "true" if is_pure(table, raw) else "false"))
        reg.append("};")
        reg.append("")
        reg.append("const CompiledBlock* compiled_blocks(size_t& count) {")
        reg.append("    count = sizeof(g_blocks) / sizeof(g_blocks[0]);")
        reg.append("    return g_blocks;")
        reg.append("}")
    else:
        reg.append("const CompiledBlock* compiled_blocks(size_t& count) {")
        reg.append("    count = 0;")
        reg.append("    return nullptr;")
        reg.append("}")
    reg.append("")
    reg.append("}")
    reg.append("")
    open(os.path.join(out_dir, "spu_registry.cpp"), "w", newline="\n").write("\n".join(reg))
    print("%d blocks, %d instructions, %d files" % (len(blocks), sum(len(r) for _, r in blocks), files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
