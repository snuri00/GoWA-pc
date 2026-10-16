import collections
import hashlib
import json
import os
import struct
import sys
from inventory import Elf

PROLOGUE = b"\x24\x00\x40\x80"


def is_guid(words):
    return all((w & 0xFE00007F) == 0x42000002 for w in words)


def spu_elf_span(d, off):
    phoff = struct.unpack(">I", d[off + 0x1C:off + 0x20])[0]
    phn = struct.unpack(">H", d[off + 0x2C:off + 0x2E])[0]
    shoff = struct.unpack(">I", d[off + 0x20:off + 0x24])[0]
    shn = struct.unpack(">H", d[off + 0x30:off + 0x32])[0]
    end = phoff + phn * 32
    for k in range(phn):
        _, p_off, _, _, p_filesz = struct.unpack(">5I", d[off + phoff + k * 32:off + phoff + k * 32 + 20])
        end = max(end, p_off + p_filesz)
    if shoff and shn:
        end = max(end, shoff + shn * 40)
    return end


def seg_end(elf, addr):
    for t, _, _, va, _, fs, _, _ in elf.phdrs:
        if t == 1 and va <= addr < va + fs:
            return va + fs
    return None


def scan(elf, region):
    lo, hi = region
    d = elf.d
    starts = {}
    for t, _, o, va, _, fs, _, _ in elf.phdrs:
        if t != 1 or not fs:
            continue
        a, b = max(va, lo), min(va + fs, hi)
        if a >= b:
            continue
        blob = d[o + a - va:o + b - va]
        for k in range(0, len(blob) - 15, 16):
            addr = a + k
            if blob[k:k + 4] == b"\x7fELF" and struct.unpack(">H", blob[k + 0x12:k + 0x14])[0] == 23:
                starts[addr] = ("elf", spu_elf_span(blob, k))
            elif is_guid(struct.unpack_from(">4I", blob, k)):
                starts.setdefault(addr, ("guid", None))
    refs = collections.defaultdict(list)
    for t, _, o, va, _, fs, _, _ in elf.phdrs:
        if t != 1 or not fs:
            continue
        blob = d[o:o + fs]
        for k in range(0, len(blob) - 3, 4):
            v = struct.unpack_from(">I", blob, k)[0]
            if lo <= v < hi and v % 16 == 0:
                refs[v].append(va + k)
    for v in refs:
        if seg_end(elf, v):
            starts.setdefault(v, ("blob", None))
    order = []
    covered = 0
    for addr in sorted(starts):
        if addr < covered:
            continue
        order.append(addr)
        kind, span = starts[addr]
        if kind == "elf":
            covered = addr + span
    images = []
    for n, addr in enumerate(order):
        kind, span = starts[addr]
        nxt = order[n + 1] if n + 1 < len(order) else hi
        size = min(span if span else nxt - addr, seg_end(elf, addr) - addr)
        data = elf.read(addr, size)
        images.append({
            "addr": addr,
            "kind": kind,
            "size": size,
            "guid": data[:16].hex() if kind == "guid" else None,
            "prologues": sum(1 for k in range(0, len(data) - 3, 4) if data[k:k + 4] == PROLOGUE),
            "refs": [hex(r) for r in refs.get(addr, [])][:16],
            "sha1": hashlib.sha1(data).hexdigest(),
            "data": data,
        })
    return images


def main(elf_path, lo, hi, out_dir, mode="all"):
    elf = Elf(elf_path)
    images = scan(elf, (int(lo, 16), int(hi, 16)))
    if mode == "noblob":
        images = [im for im in images if im["kind"] != "blob"]
    os.makedirs(out_dir, exist_ok=True)
    meta = []
    for im in images:
        name = f"{im['addr']:08x}_{im['kind']}.bin"
        open(os.path.join(out_dir, name), "wb").write(im.pop("data"))
        im["file"] = name
        im["addr"] = hex(im["addr"])
        meta.append(im)
        print(f"{im['addr']} {im['kind']:5} {im['size']:>8} prologues {im['prologues']:>4} refs {len(im['refs']):>2} {im['guid'] or ''}")
    json.dump(meta, open(os.path.join(out_dir, "images.json"), "w"), indent=1)
    print(len(meta), "images,", sum(m["prologues"] for m in meta), "prologues")


if __name__ == "__main__":
    main(*sys.argv[1:6])
