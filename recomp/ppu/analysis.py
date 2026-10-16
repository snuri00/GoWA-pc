import collections
import json
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from inventory import Elf, opd, prx_param, imports
from isa import mnemonic


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


class Image:
    def __init__(self, path, nids=None):
        self.elf = Elf(path)
        prx = prx_param(self.elf)
        self.imports = []
        for m in imports(self.elf, prx, nids or {}):
            for f in m["functions"]:
                self.imports.append({"module": m["module"], "nid": f["nid"], "name": f["name"], "stub": int(f["stub_target"], 16)})
        self.stubs = {imp["stub"]: i for i, imp in enumerate(self.imports)}
        stub_lo = min(self.stubs) if self.stubs else None
        execs = sorted((s[3], s[3] + s[5]) for s in self.elf.shdrs if s[2] & 4 and s[5])
        code = [r for r in execs if stub_lo is None or not (r[0] <= stub_lo < r[1])]
        self.text_lo = code[0][0]
        self.text_hi = max(b for _, b in code)
        self.text_size = self.text_hi - self.text_lo
        self.text = self.elf.read(self.text_lo, self.text_size)
        o = opd(self.elf)
        base = int(o["addr"], 16)
        raw = self.elf.read(base, o["count"] * 8)
        self.opd = [struct.unpack_from(">II", raw, i * 8) for i in range(o["count"])]
        self.toc = self.opd[0][1]

    def word(self, addr):
        return struct.unpack_from(">I", self.text, addr - self.text_lo)[0]

    def in_text(self, addr):
        return self.text_lo <= addr < self.text_hi and addr % 4 == 0

    def toc_word(self, disp):
        return self.elf.u32((self.toc + disp) & 0xFFFFFFFF)


def branch_target(pc, w):
    op = w >> 26
    if op == 18:
        li = sext(w & 0x03FFFFFC, 26)
        return (li if w & 2 else pc + li) & 0xFFFFFFFF
    if op == 16:
        bd = sext(w & 0xFFFC, 16)
        return (bd if w & 2 else pc + bd) & 0xFFFFFFFF
    return None


def bo_always(w):
    return ((w >> 21) & 0x14) == 0x14


def jump_table(img, bctr):
    window = [(a, img.word(a)) for a in range(max(img.text_lo, bctr - 4 * 16), bctr, 4)]
    base = None
    count = None
    for a, w in reversed(window):
        op = w >> 26
        if base is None and op == 32 and (w >> 16) & 0x1F == 2:
            base = img.toc_word(sext(w & 0xFFFF, 16))
        if count is None and op == 10:
            count = (w & 0xFFFF) + 1
    if base is None or base != bctr + 4:
        return None
    limit = count if count is not None else 1024
    targets = []
    table_end = img.text_hi
    while len(targets) < limit and base + len(targets) * 4 < table_end:
        v = sext(img.word(base + len(targets) * 4), 32)
        t = (base + v) & 0xFFFFFFFF
        if not img.in_text(t) or abs(v) > 0x100000:
            break
        targets.append(t)
        if t > base:
            table_end = min(table_end, t)
    return {"base": base, "count": len(targets), "bound": count, "targets": targets}


class Function:
    def __init__(self, entry):
        self.entry = entry
        self.insns = set()
        self.calls = set()
        self.tail_calls = set()
        self.jump_tables = {}
        self.indirect_jumps = []
        self.indirect_calls = 0
        self.syscalls = []
        self.errors = []
        self.data = set()
        self.import_calls = set()


def explore(img, entry, entries):
    f = Function(entry)
    work = [entry]
    while work:
        pc = work.pop()
        while True:
            if pc in f.insns:
                break
            if not img.in_text(pc):
                f.errors.append(("out_of_text", pc))
                break
            if pc != entry and pc in entries:
                f.tail_calls.add(pc)
                break
            w = img.word(pc)
            m = mnemonic(w)
            if m is None:
                f.errors.append(("invalid", pc, w))
                break
            f.insns.add(pc)
            op = w >> 26
            if op == 18:
                t = branch_target(pc, w)
                if t in img.stubs:
                    f.import_calls.add(t)
                    if w & 1:
                        pc += 4
                        continue
                    break
                if w & 1:
                    f.calls.add(t)
                    pc += 4
                    continue
                if t in entries and t != entry:
                    f.tail_calls.add(t)
                else:
                    work.append(t)
                break
            if op == 16:
                t = branch_target(pc, w)
                if w & 1:
                    f.calls.add(t)
                    pc += 4
                    continue
                if t in entries and t != entry:
                    f.tail_calls.add(t)
                else:
                    work.append(t)
                if bo_always(w):
                    break
                pc += 4
                continue
            if op == 19 and m in ("bclr", "bcctr"):
                if w & 1:
                    f.indirect_calls += 1
                    pc += 4
                    continue
                if m == "bcctr":
                    jt = jump_table(img, pc)
                    if jt:
                        f.jump_tables[pc] = jt
                        f.data.update(range(jt["base"], jt["base"] + jt["count"] * 4, 4))
                        for t in jt["targets"]:
                            work.append(t)
                    else:
                        f.indirect_jumps.append(pc)
                if bo_always(w):
                    break
                pc += 4
                continue
            if w == 0x44000002:
                f.syscalls.append(pc)
            if w == 0x7FE00008:
                break
            pc += 4
    return f


def is_prologue(w):
    return (w & 0xFFFF0003) == 0xF8210001 or w == 0x7C0802A6


def looks_like_function(img, a, b):
    ws = [img.word(x) for x in range(a, b, 4)]
    if any(is_prologue(w) for w in ws[:8]):
        return True
    return all(mnemonic(w) for w in ws) and ws[-1] == 0x4E800020 and not any(((w >> 23) & 0xFF) in range(0x70, 0x90) and (w & 0xFFFF) == 0 for w in ws)


def discover(img, extra=()):
    entries = {c for c, _ in img.opd if img.in_text(c)} | set(extra)
    funcs = {}
    pending = set(entries)
    while pending:
        for e in sorted(pending):
            funcs[e] = explore(img, e, entries)
        new = set()
        for f in funcs.values():
            for t in f.calls | f.tail_calls:
                if img.in_text(t) and t not in entries:
                    new.add(t)
        entries |= new
        stale = {e for e, f in funcs.items() if any(i in new for i in f.insns if i != e)}
        pending = new | stale
    return funcs, entries


def islands(img, funcs):
    covered = set()
    for f in funcs.values():
        covered |= f.insns
        covered |= f.data
    out = []
    run = None
    for a in range(img.text_lo, img.text_hi, 4):
        w = img.word(a)
        if a in covered or w == 0 or w == 0x60000000:
            if run and run[1] - run[0] >= 16:
                out.append(run)
            run = None
            continue
        if mnemonic(w):
            run = [run[0], a + 4] if run else [a, a + 4]
        else:
            if run and run[1] - run[0] >= 16:
                out.append(run)
            run = None
    return out, covered


def main(elf_path, out_path):
    img = Image(elf_path)
    funcs, entries = discover(img)
    isl, covered = islands(img, funcs)
    extra = set()
    while True:
        found = {a for a, b in isl if looks_like_function(img, a, b)} - extra
        if not found:
            break
        extra |= found
        funcs, entries = discover(img, extra)
        isl, covered = islands(img, funcs)
    print(f"prologue-started islands promoted to functions: {len(extra)}")
    ninsn = sum(len(f.insns) for f in funcs.values())
    owners = collections.Counter()
    for f in funcs.values():
        for i in f.insns:
            owners[i] += 1
    shared = sum(1 for v in owners.values() if v > 1)
    errs = [(hex(f.entry), e[0], hex(e[1])) for f in funcs.values() for e in f.errors]
    jts = sum(len(f.jump_tables) for f in funcs.values())
    ind = sum(len(f.indirect_jumps) for f in funcs.values())
    print(f"toc {img.toc:#x} opd {len(img.opd)} functions {len(funcs)} (from bl/b {len(entries) - len({c for c, _ in img.opd})})")
    print(f"instructions reached {ninsn} unique {len(owners)} shared {shared} of {img.text_size // 4} words")
    print(f"jump tables {jts} unresolved bctr {ind} indirect calls {sum(f.indirect_calls for f in funcs.values())} syscalls {sum(len(f.syscalls) for f in funcs.values())}")
    print(f"errors {len(errs)}: {errs[:10]}")
    print(f"uncovered code-like islands >=4 insns: {len(isl)} totalling {sum((b - a) // 4 for a, b in isl)} insns; first {[(hex(a), (b - a) // 4) for a, b in isl[:10]]}")
    out = {
        "toc": hex(img.toc),
        "functions": [
            {
                "entry": hex(f.entry),
                "start": hex(min(f.insns)) if f.insns else hex(f.entry),
                "end": hex(max(f.insns) + 4) if f.insns else hex(f.entry),
                "insns": len(f.insns),
                "calls": sorted(hex(c) for c in f.calls),
                "tail_calls": sorted(hex(c) for c in f.tail_calls),
                "jump_tables": {hex(k): {"base": hex(v["base"]), "count": v["count"]} for k, v in f.jump_tables.items()},
                "indirect_jumps": [hex(x) for x in f.indirect_jumps],
                "indirect_calls": f.indirect_calls,
                "syscalls": [hex(x) for x in f.syscalls],
                "errors": [[e[0]] + [hex(x) for x in e[1:]] for e in f.errors],
            }
            for f in sorted(funcs.values(), key=lambda f: f.entry)
        ],
        "islands": [[hex(a), hex(b)] for a, b in isl],
    }
    json.dump(out, open(out_path, "w"), indent=0)


if __name__ == "__main__":
    main(*sys.argv[1:3])
