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


RRR = set()

NO_WRITE = {
    "op_stqd", "op_stqx", "op_stqa", "op_stqr", "op_br", "op_bra", "op_brz", "op_brnz", "op_brhz", "op_brhnz",
    "op_bi", "op_biz", "op_binz", "op_bihz", "op_bihnz", "op_iret", "op_hbr", "op_hbra", "op_hbrr",
    "op_nop", "op_lnop", "op_sync", "op_dsync", "op_mtspr",
}


def decode_table():
    src = open(INTERP, encoding="utf-8").read()
    table = ["op_illegal"] * 2048
    for m in re.finditer(r"set\((0x[0-9A-Fa-f]+),\s*(\d+),\s*(op_\w+)\)", src):
        opcode, bits, name = int(m.group(1), 16), int(m.group(2)), m.group(3)
        if bits == 4:
            RRR.add(name)
        shift = 11 - bits
        for k in range(1 << shift):
            table[(opcode << shift) | k] = name
    return table


def reg_sets(table, raw):
    reads, writes = set(), set()
    for r in raw:
        w = struct.unpack(">I", struct.pack("<I", r))[0]
        name = table[w >> 21]
        for shift in (0, 7, 14, 21):
            reads.add((w >> shift) & 0x7F)
        if name in NO_WRITE:
            continue
        writes.add((w >> 21) & 0x7F if name in RRR else w & 0x7F)
    return sorted(reads | writes), sorted(writes)


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


def emit_pure_block(table, idx, lsa, raw, out):
    reads, writes = reg_sets(table, raw)
    out.append("bool b%d(Context& c0, Host& h) {" % idx)
    out.append("    (void)h;")
    out.append("    Context c;")
    out.append("    c.ls = c0.ls;")
    out.append("    c.line_epoch = c0.line_epoch;")
    out.append("    c.epoch = c0.epoch;")
    out.append("    c.pc = c0.pc;")
    out.append("    c.srr0 = c0.srr0;")
    out.append("    c.interrupts_enabled = c0.interrupts_enabled;")
    out.append("    c.irq_check = c0.irq_check;")
    out.append("    c.halted = c0.halted;")
    for r in reads:
        out.append("    c.gpr[%d] = c0.gpr[%d];" % (r, r))
    n = len(raw)
    last_branch = False
    for k, r in enumerate(raw):
        w = struct.unpack(">I", struct.pack("<I", r))[0]
        pc = (lsa + k * 4) & 0x3FFFC
        name = table[w >> 21]
        if name in CONTROL:
            out.append("    c.pc = 0x%x;" % ((pc + 4) & 0x3FFFC))
            out.append("    %s(c, h, 0x%08xu, 0x%xu);" % (name, w, pc))
            last_branch = True
        else:
            out.append("    %s(c, h, 0x%08xu, 0x%xu);" % (name, w, pc))
            last_branch = False
    if not last_branch:
        out.append("    c.pc = 0x%x;" % ((lsa + n * 4) & 0x3FFFC))
    for r in writes:
        out.append("    c0.gpr[%d] = c.gpr[%d];" % (r, r))
    out.append("    c0.pc = c.pc;")
    out.append("    c0.interrupts_enabled = c.interrupts_enabled;")
    out.append("    c0.irq_check = c.irq_check;")
    out.append("    return true;")
    out.append("}")
    out.append("")


REGION_MAX = 64
REGION_BUDGET = 4096
COND_BRANCH = {"op_brz", "op_brnz", "op_brhz", "op_brhnz", "op_biz", "op_binz", "op_bihz", "op_bihnz"}


def word_of(r):
    return struct.unpack(">I", struct.pack("<I", r))[0]


def s16(w):
    v = (w >> 7) & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def successors(table, lsa, raw):
    n = len(raw)
    w = word_of(raw[-1])
    name = table[w >> 21]
    last_pc = (lsa + (n - 1) * 4) & 0x3FFFC
    fall = (lsa + n * 4) & 0x3FFFC
    if name in ("op_br",):
        return [(last_pc + (s16(w) << 2)) & 0x3FFFC]
    if name in ("op_bra",):
        return [(s16(w) << 2) & 0x3FFFC]
    if name in ("op_brsl",):
        return [(last_pc + (s16(w) << 2)) & 0x3FFFC]
    if name in ("op_brasl",):
        return [(s16(w) << 2) & 0x3FFFC]
    if name in ("op_brz", "op_brnz", "op_brhz", "op_brhnz"):
        return [(last_pc + (s16(w) << 2)) & 0x3FFFC, fall]
    if name in ("op_biz", "op_binz", "op_bihz", "op_bihnz"):
        return [fall]
    if name in ("op_bi", "op_bisl", "op_iret"):
        return []
    return [fall]


def build_regions(table, blocks):
    pure = [is_pure(table, raw) for _, raw in blocks]
    by_lsa = {}
    for i, (lsa, _) in enumerate(blocks):
        by_lsa.setdefault(lsa, []).append(i)
    region_of = [-1] * len(blocks)
    regions = []
    for seed in range(len(blocks)):
        if not pure[seed] or region_of[seed] >= 0:
            continue
        members = [seed]
        region_of[seed] = len(regions)
        queue = [seed]
        while queue and len(members) < REGION_MAX:
            i = queue.pop(0)
            lsa, raw = blocks[i]
            for t in successors(table, lsa, raw):
                cands = by_lsa.get(t, [])
                if len(cands) != 1:
                    continue
                j = cands[0]
                if not pure[j] or region_of[j] >= 0:
                    continue
                region_of[j] = len(regions)
                members.append(j)
                queue.append(j)
                if len(members) >= REGION_MAX:
                    break
        regions.append(members)
    keep = [m for m in regions if len(m) >= 2]
    region_of = [-1] * len(blocks)
    for k, m in enumerate(keep):
        for i in m:
            region_of[i] = k
    return keep, region_of


def emit_region(table, k, members, blocks, out):
    reads, writes = set(), set()
    for i in members:
        r, w = reg_sets(table, blocks[i][1])
        reads.update(r)
        writes.update(w)
    out.append("bool r%d(Context& c0, Host& h) {" % k)
    out.append("    (void)h;")
    out.append("    Context c;")
    out.append("    switch (c0.pc) {")
    for i in members:
        out.append("    case 0x%x: break;" % blocks[i][0])
    out.append("    default: c0.executed = 0; return true;")
    out.append("    }")
    for f in ("ls", "line_epoch", "epoch", "all_epoch", "block_valid", "pc", "srr0", "interrupts_enabled", "irq_check", "halted"):
        out.append("    c.%s = c0.%s;" % (f, f))
    for r in sorted(reads | writes):
        out.append("    c.gpr[%d] = c0.gpr[%d];" % (r, r))
    out.append("    uint32_t done = 0;")
    out.append("    switch (c.pc) {")
    for i in members:
        out.append("    case 0x%x: goto B%d;" % (blocks[i][0], i))
    out.append("    default: goto EXIT;")
    out.append("    }")
    for i in members:
        lsa, raw = blocks[i]
        n = len(raw)
        out.append("B%d:" % i)
        last_branch = False
        for kk, r in enumerate(raw):
            w = word_of(r)
            pc = (lsa + kk * 4) & 0x3FFFC
            name = table[w >> 21]
            if name in CONTROL:
                out.append("    c.pc = 0x%x;" % ((pc + 4) & 0x3FFFC))
                out.append("    %s(c, h, 0x%08xu, 0x%xu);" % (name, w, pc))
                last_branch = True
            else:
                out.append("    %s(c, h, 0x%08xu, 0x%xu);" % (name, w, pc))
                last_branch = False
        if not last_branch:
            out.append("    c.pc = 0x%x;" % ((lsa + n * 4) & 0x3FFFC))
        out.append("    done += %d;" % n)
        out.append("    goto NEXT;")
    out.append("NEXT:")
    out.append("    if (c.irq_check || done >= %d) goto EXIT;" % REGION_BUDGET)
    out.append("    switch (c.pc) {")
    for i in members:
        lsa, raw = blocks[i]
        out.append("    case 0x%x: if (region_valid(c, %du, 0x%xu, %du, w%d)) goto B%d; goto EXIT;" % (lsa, i, lsa, len(raw), i, i))
    out.append("    default: goto EXIT;")
    out.append("    }")
    out.append("EXIT:")
    for r in sorted(writes):
        out.append("    c0.gpr[%d] = c.gpr[%d];" % (r, r))
    out.append("    c0.pc = c.pc;")
    out.append("    c0.interrupts_enabled = c.interrupts_enabled;")
    out.append("    c0.irq_check = c.irq_check;")
    out.append("    c0.executed = done;")
    out.append("    return true;")
    out.append("}")
    out.append("")


STATE_FIELDS = ("pc", "srr0", "interrupts_enabled", "irq_check", "halted")


def emit_local_block(table, idx, lsa, raw, out):
    reads, writes = reg_sets(table, raw)
    regs = sorted(set(reads) | set(writes))
    out.append("bool b%d(Context& c0, Host& h) {" % idx)
    out.append("    Context c;")
    for f in ("ls", "line_epoch", "epoch", "all_epoch", "block_valid") + STATE_FIELDS:
        out.append("    c.%s = c0.%s;" % (f, f))
    for r in regs:
        out.append("    c.gpr[%d] = c0.gpr[%d];" % (r, r))

    def flush(skip=()):
        for r in writes:
            if r not in skip:
                out.append("    c0.gpr[%d] = c.gpr[%d];" % (r, r))

    n = len(raw)
    for k, r in enumerate(raw):
        w = word_of(r)
        pc = (lsa + k * 4) & 0x3FFFC
        name = table[w >> 21]
        if name not in CONTROL:
            out.append("    %s(c, h, 0x%08xu, 0x%xu);" % (name, w, pc))
            continue
        ops = sorted({w & 0x7F, (w >> 7) & 0x7F, (w >> 14) & 0x7F})
        if k == n - 1:
            out.append("    c.pc = 0x%x;" % ((pc + 4) & 0x3FFFC))
            flush()
            for o in ops:
                if o not in writes:
                    out.append("    c0.gpr[%d] = c.gpr[%d];" % (o, o))
            for f in STATE_FIELDS:
                out.append("    c0.%s = c.%s;" % (f, f))
            out.append("    return %s(c0, h, 0x%08xu, 0x%xu);" % (name, w, pc))
            out.append("}")
            out.append("")
            return
        out.append("    c.pc = 0x%x;" % ((pc + 4) & 0x3FFFC))
        for o in ops:
            out.append("    c0.gpr[%d] = c.gpr[%d];" % (o, o))
        for f in STATE_FIELDS:
            out.append("    c0.%s = c.%s;" % (f, f))
        out.append("    {")
        out.append("        bool ok = %s(c0, h, 0x%08xu, 0x%xu);" % (name, w, pc))
        for o in ops:
            out.append("        c.gpr[%d] = c0.gpr[%d];" % (o, o))
        for f in STATE_FIELDS:
            out.append("        c.%s = c0.%s;" % (f, f))
        out.append("        if (!ok || c.pc != 0x%x) {" % ((pc + 4) & 0x3FFFC))
        for r2 in writes:
            out.append("            c0.gpr[%d] = c.gpr[%d];" % (r2, r2))
        out.append("            return ok;")
        out.append("        }")
        out.append("    }")
    out.append("    c.pc = 0x%x;" % ((lsa + n * 4) & 0x3FFFC))
    flush()
    for f in STATE_FIELDS:
        out.append("    c0.%s = c.%s;" % (f, f))
    out.append("    return true;")
    out.append("}")
    out.append("")


def emit_block(table, idx, lsa, raw, out):
    if not os.environ.get("GOWA_SPU_NO_LOCAL") and is_pure(table, raw):
        emit_pure_block(table, idx, lsa, raw, out)
        return
    if os.environ.get("GOWA_SPU_LOCAL_IMPURE"):
        emit_local_block(table, idx, lsa, raw, out)
        return
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
    use_regions = not os.environ.get("GOWA_SPU_NO_REGIONS")
    regions, region_of = build_regions(table, blocks) if use_regions else ([], [-1] * len(blocks))
    per_file = 60
    rfiles = (len(regions) + per_file - 1) // per_file
    for f in range(rfiles):
        out = ['#include "ops_impl.h"', "", "namespace spu::gen {", ""]
        needed = set()
        for k in range(f * per_file, min(len(regions), (f + 1) * per_file)):
            needed.update(regions[k])
        for i in sorted(needed):
            out.append("extern const uint32_t w%d[];" % i)
        out.append("")
        for k in range(f * per_file, min(len(regions), (f + 1) * per_file)):
            emit_region(table, k, regions[k], blocks, out)
        out.append("}")
        out.append("")
        open(os.path.join(out_dir, "spu_r%03d.cpp" % f), "w", newline="\n").write("\n".join(out))
    reg = ['#include "spu/aot.h"', "", "namespace spu::gen {", ""]
    for i in range(len(blocks)):
        reg.append("bool b%d(Context& c, Host& h);" % i)
    for k in range(len(regions)):
        reg.append("bool r%d(Context& c, Host& h);" % k)
    reg.append("")
    for i, (lsa, raw) in enumerate(blocks):
        reg.append("extern const uint32_t w%d[] = {%s};" % (i, ", ".join("0x%08xu" % r for r in raw)))
    reg.append("")
    reg.append("}")
    reg.append("")
    reg.append("namespace spu {")
    reg.append("")
    if blocks:
        reg.append("const CompiledBlock g_blocks[] = {")
        for i, (lsa, raw) in enumerate(blocks):
            reg.append("    {0x%xu, %du, gen::w%d, gen::b%d, %s, %s}," % (lsa, len(raw), i, i, "true" if is_pure(table, raw) else "false", ("gen::r%d" % region_of[i]) if region_of[i] >= 0 else "nullptr"))
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
    print("%d blocks, %d instructions, %d files, %d regions covering %d blocks" % (len(blocks), sum(len(r) for _, r in blocks), files, len(regions), sum(len(m) for m in regions)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
