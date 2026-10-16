import collections
import json
import os
import struct
import sys

PT_NAMES = {1: "LOAD", 7: "TLS", 0x60000001: "PROC_PARAM", 0x60000002: "PRX_PARAM"}
X31 = {20: "lwarx", 84: "ldarx", 150: "stwcx.", 214: "stdcx.", 1014: "dcbz", 371: "mftb", 598: "sync", 854: "eieio", 278: "dcbt", 246: "dcbtst"}
X31_VMX_UNALIGNED = {519, 551, 583, 615, 647, 679, 711, 743, 775, 807}
X63 = {711: "mtfsf", 134: "mtfsfi", 70: "mtfsb0", 38: "mtfsb1", 583: "mffs"}
SITE_KINDS = {"lwarx", "ldarx", "dcbz", "mftb", "mftb(mfspr)", "sc", "mtvscr", "mtfsf", "mtfsfi", "mtfsb0", "mtfsb1"}


class Elf:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        self.entry, phoff, shoff = struct.unpack(">QQQ", d[0x18:0x30])
        phes, phn, shes, shn = struct.unpack(">HHHH", d[0x36:0x3E])
        self.phdrs = [struct.unpack(">IIQQQQQQ", d[phoff + i * phes:phoff + i * phes + 56]) for i in range(phn)]
        self.shdrs = [struct.unpack(">IIQQQQIIQQ", d[shoff + i * shes:shoff + i * shes + 64]) for i in range(shn)] if shoff else []

    def off(self, addr):
        for t, _, o, va, _, fs, _, _ in self.phdrs:
            if t == 1 and va <= addr < va + fs:
                return o + addr - va
        return None

    def read(self, addr, n):
        o = self.off(addr)
        return None if o is None else self.d[o:o + n]

    def u32(self, addr):
        return struct.unpack(">I", self.read(addr, 4))[0]

    def cstr(self, addr):
        o = self.off(addr)
        return self.d[o:self.d.index(b"\0", o)].decode("latin1")

    def phdr(self, ptype):
        return next((p for p in self.phdrs if p[0] == ptype), None)

    def code(self):
        for t, f, o, va, _, fs, _, _ in self.phdrs:
            if t == 1 and f & 1 and fs:
                yield va, self.d[o:o + fs]


def process_param(elf):
    p = elf.phdr(0x60000001)
    if not p:
        return None
    b = elf.read(p[3], p[5])
    keys = ["size", "magic", "version", "sdk_version", "primary_prio", "primary_stacksize", "malloc_pagesize", "ppc_seg", "crash_dump_param_addr"]
    vals = struct.unpack(">" + "I" * (len(b) // 4), b)
    return {k: hex(v) for k, v in zip(keys, vals)}


def prx_param(elf):
    p = elf.phdr(0x60000002)
    if not p:
        return None
    _, magic, version, _, libent_start, libent_end, libstub_start, libstub_end = struct.unpack(">8I", elf.read(p[3], 32))
    return {"magic": hex(magic), "version": hex(version), "libent": (libent_start, libent_end), "libstub": (libstub_start, libstub_end)}


def imports(elf, prx, nids):
    out = []
    addr, end = prx["libstub"]
    while addr < end:
        b = elf.read(addr, 0x2C)
        size = b[0]
        num_func, num_var, num_tls = struct.unpack(">HHH", b[6:12])
        name_p, nid_p, stub_p, vnid_p, _ = struct.unpack(">5I", b[0x10:0x24])
        name = elf.cstr(name_p)
        table = nids.get(name, {})
        funcs = []
        for i in range(num_func):
            n = elf.u32(nid_p + i * 4)
            funcs.append({"nid": f"{n:08X}", "name": table.get(f"{n:08X}"), "stub_slot": hex(stub_p + i * 4), "stub_target": hex(elf.u32(stub_p + i * 4))})
        variables = []
        for i in range(num_var):
            n = elf.u32(vnid_p + i * 4)
            variables.append({"nid": f"{n:08X}", "name": table.get(f"{n:08X}")})
        out.append({"module": name, "functions": funcs, "variables": variables, "tls": num_tls})
        addr += size
    return out


def exports(elf, prx, nids):
    out = []
    addr, end = prx["libent"]
    while addr < end:
        b = elf.read(addr, 0x1C)
        size = b[0]
        attr = struct.unpack(">H", b[4:6])[0]
        num_func, num_var, num_tls = struct.unpack(">HHH", b[6:12])
        name_p, nid_p, addr_p = struct.unpack(">3I", b[0x10:0x1C])
        name = elf.cstr(name_p) if name_p else ""
        ents = []
        for i in range(num_func + num_var + num_tls):
            n = elf.u32(nid_p + i * 4)
            ents.append({"nid": f"{n:08X}", "name": nids.get(name, {}).get(f"{n:08X}"), "addr": hex(elf.u32(addr_p + i * 4))})
        out.append({"module": name, "attr": hex(attr), "entries": ents})
        addr += size
    return out


def opd(elf):
    for sh in elf.shdrs:
        addr, size = sh[3], sh[5]
        if addr <= elf.entry < addr + size and size % 8 == 0:
            b = elf.read(addr, size)
            ents = [struct.unpack(">II", b[i:i + 8]) for i in range(0, size, 8)]
            tocs = collections.Counter(t for _, t in ents)
            return {"addr": hex(addr), "count": len(ents), "unique_code": len({c for c, _ in ents}), "tocs": {hex(k): v for k, v in tocs.most_common(8)}, "entry_opd": hex(elf.entry)}
    return None


def scan_code(elf):
    counts = collections.Counter()
    sites = collections.defaultdict(list)
    bl_targets = set()

    def hit(name, pc):
        counts[name] += 1
        if name in SITE_KINDS:
            sites[name].append(pc)

    for base, blob in elf.code():
        for i in range(0, len(blob) - 3, 4):
            w = struct.unpack_from(">I", blob, i)[0]
            op = w >> 26
            pc = base + i
            if w == 0x44000002:
                hit("sc", pc)
            elif op == 18 and w & 1:
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                bl_targets.add((li if w & 2 else pc + li) & 0xFFFFFFFF)
                counts["bl"] += 1
            elif op == 19:
                xo = (w >> 1) & 0x3FF
                if xo == 16 and (w >> 21) & 0x14 == 0x14:
                    counts["blrl" if w & 1 else "blr"] += 1
                elif xo == 528:
                    counts["bctrl" if w & 1 else "bctr"] += 1
            elif op == 31:
                xo = (w >> 1) & 0x3FF
                name = X31.get(xo)
                if xo == 339 and (((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5)) in (268, 269):
                    name = "mftb(mfspr)"
                if xo == 598 and (w >> 21) & 3 == 1:
                    name = "lwsync"
                if xo in X31_VMX_UNALIGNED:
                    name = "lvlx/lvrx/stvlx/stvrx"
                if name:
                    hit(name, pc)
            elif op == 4:
                vx = w & 0x7FF
                if vx == 1604:
                    hit("mtvscr", pc)
                elif vx == 1540:
                    counts["mfvscr"] += 1
                elif w & 0x3F == 46:
                    counts["vmaddfp"] += 1
                elif vx == 266:
                    counts["vrefp"] += 1
                elif vx == 330:
                    counts["vrsqrtefp"] += 1
            elif op == 63:
                xo = (w >> 1) & 0x3FF
                if xo in X63:
                    hit(X63[xo], pc)
                a = (w >> 1) & 0x1F
                if a in (28, 29, 30, 31):
                    counts["fmadd_family"] += 1
                elif a == 26:
                    counts["frsqrte"] += 1
                elif xo in (14, 15, 814, 815):
                    counts["fctiw/fctid"] += 1
            elif op == 59:
                a = (w >> 1) & 0x1F
                if a in (28, 29, 30, 31):
                    counts["fmadds_family"] += 1
                elif a == 24:
                    counts["fres"] += 1
    counts["bl_unique_targets"] = len(bl_targets)
    return dict(counts), sites


def sc_numbers(elf, sc_sites):
    nums = collections.Counter()
    for pc in sc_sites:
        n = None
        for back in range(1, 12):
            b = elf.read(pc - back * 4, 4)
            if b is None:
                break
            w = struct.unpack(">I", b)[0]
            if w >> 26 == 14 and (w >> 21) & 0x1F == 11 and (w >> 16) & 0x1F == 0:
                n = w & 0xFFFF
                break
        nums[n] += 1
    return {str(k): v for k, v in sorted(nums.items(), key=lambda kv: (kv[0] is None, kv[0] or 0))}


def spu_images(elf):
    d = elf.d
    out = []
    i = d.find(b"\x7fELF", 4)
    while i >= 0:
        if d[i + 4] == 1 and struct.unpack(">H", d[i + 0x12:i + 0x14])[0] == 23:
            entry, phoff = struct.unpack(">II", d[i + 0x18:i + 0x20])
            phn = struct.unpack(">H", d[i + 0x2C:i + 0x2E])[0]
            segs = [struct.unpack(">8I", d[i + phoff + k * 32:i + phoff + k * 32 + 32]) for k in range(phn)]
            span = max((s[1] + s[4] for s in segs), default=0)
            out.append({"kind": "elf", "file_off": hex(i), "entry": hex(entry), "phnum": phn, "span": span})
        i = d.find(b"\x7fELF", i + 4)
    for t, f, o, va, _, fs, _, _ in elf.phdrs:
        if t != 1 or f & 1 or not fs:
            continue
        blob = d[o:o + fs]
        for k in range(0, len(blob) - 15, 16):
            ws = struct.unpack_from(">4I", blob, k)
            if all((w & 0xFE00007F) == 0x42000002 for w in ws):
                out.append({"kind": "guid", "addr": hex(va + k), "guid": "".join(f"{w:08x}" for w in ws)})
    return out


def strings(elf, needles):
    found = collections.defaultdict(set)
    run = bytearray()
    for b in elf.d:
        if 32 <= b < 127:
            run.append(b)
            continue
        if len(run) >= 6:
            s = run.decode()
            low = s.lower()
            for n in needles:
                if n in low:
                    found[n].add(s)
        run.clear()
    return {k: sorted(v)[:300] for k, v in found.items()}


def main(elf_path, nid_db_path, out_path):
    elf = Elf(elf_path)
    nids = json.load(open(nid_db_path))
    prx = prx_param(elf)
    inv = {"file": os.path.basename(elf_path), "size": len(elf.d), "entry": hex(elf.entry)}
    inv["segments"] = [{"type": PT_NAMES.get(t, hex(t)), "flags": hex(f), "vaddr": hex(va), "filesz": hex(fs), "memsz": hex(ms)} for t, f, _, va, _, fs, ms, _ in elf.phdrs]
    inv["sections"] = [{"type": hex(s[1]), "flags": hex(s[2]), "addr": hex(s[3]), "size": hex(s[5])} for s in elf.shdrs]
    inv["process_param"] = process_param(elf)
    inv["prx_param"] = {k: ([hex(x) for x in v] if isinstance(v, tuple) else v) for k, v in prx.items()} if prx else None
    inv["imports"] = imports(elf, prx, nids) if prx else []
    inv["exports"] = exports(elf, prx, nids) if prx else []
    inv["opd"] = opd(elf)
    counts, sites = scan_code(elf)
    inv["code_counts"] = counts
    inv["syscalls"] = sc_numbers(elf, sites.get("sc", []))
    inv["code_sites"] = {k: [hex(x) for x in v] for k, v in sites.items()}
    inv["spu_images"] = spu_images(elf)
    inv["strings"] = strings(elf, ["thread", "spurs", "mlaa", "edge", "jobchain", "taskset", ".wad", ".psarc", "/dev_", "hdlr"])
    json.dump(inv, open(out_path, "w"), indent=1)
    nf = sum(len(m["functions"]) for m in inv["imports"])
    named = sum(1 for m in inv["imports"] for f in m["functions"] if f["name"])
    print(f"{inv['file']}: {len(elf.d)} bytes, entry {inv['entry']}")
    print(f"process_param: {inv['process_param']}")
    print(f"imports: {len(inv['imports'])} modules, {nf} functions ({named} named)")
    for m in sorted(inv["imports"], key=lambda m: -len(m["functions"])):
        print(f"  {m['module']:24} {len(m['functions']):>4} f {len(m['variables'])} v")
    print(f"exports: {[(e['module'], len(e['entries'])) for e in inv['exports']]}")
    print(f"opd: {inv['opd']}")
    print(f"code: {dict(sorted(counts.items()))}")
    print(f"syscalls via sc: {inv['syscalls']}")
    print(f"spu images: {collections.Counter(s['kind'] for s in inv['spu_images'])}")


if __name__ == "__main__":
    main(*sys.argv[1:4])
