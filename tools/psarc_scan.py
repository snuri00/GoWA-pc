import glob
import json
import os
import struct
import sys
from multiprocessing import Pool
from psarc import Psarc

MEDIA = (".TS", ".M2V", ".XVAG", ".MID", ".GTF")
PROLOGUE = b"\x24\x00\x40\x80"


def count(data, needle):
    n = 0
    i = data.find(needle)
    while i >= 0:
        if i % 4 == 0:
            n += 1
        i = data.find(needle, i + 1)
    return n


def spu_elfs(data):
    out = []
    i = data.find(b"\x7fELF")
    while i >= 0:
        if len(data) > i + 0x14 and data[i + 4] == 1 and struct.unpack(">H", data[i + 0x12:i + 0x14])[0] == 23:
            out.append(i)
        i = data.find(b"\x7fELF", i + 1)
    return out


def guids(data):
    out = []
    i = data.find(PROLOGUE)
    seen = set()
    while i >= 0:
        base = i & ~15
        for k in range(base, max(base - 0x400, -1), -16):
            if k in seen:
                break
            w = struct.unpack_from(">4I", data, k) if k + 16 <= len(data) else None
            if w and all((x & 0xFE00007F) == 0x42000002 for x in w):
                seen.add(k)
                out.append(k)
                break
        i = data.find(PROLOGUE, i + 1)
    return sorted(set(out))


def scan_archive(path):
    a = Psarc(path)
    hits = []
    for i, name, size in a.files():
        if name.upper().endswith(MEDIA):
            continue
        data = a.read_entry(i)
        p = count(data, PROLOGUE)
        e = spu_elfs(data)
        if p or e:
            hits.append({"archive": os.path.basename(path), "file": name, "size": size, "prologues": p, "spu_elfs": e[:32], "guids": guids(data)[:64]})
    return path, hits


def main(out_path, *roots):
    paths = sorted(p for r in roots for p in glob.glob(os.path.join(r, "*.PSARC")))
    results = []
    with Pool(max(1, os.cpu_count() - 2)) as pool:
        for path, hits in pool.imap_unordered(scan_archive, paths):
            results.extend(hits)
            print(f"{os.path.basename(path):28} {len(hits):>4} files with SPU signatures", flush=True)
    json.dump(results, open(out_path, "w"), indent=1)
    print("total", len(results))


if __name__ == "__main__":
    main(*sys.argv[1:])
