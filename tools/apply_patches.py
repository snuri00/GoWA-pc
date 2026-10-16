import json
import os
import struct
import sys


def segments(data):
    phoff = struct.unpack_from(">Q", data, 0x20)[0]
    count = struct.unpack_from(">H", data, 0x38)[0]
    out = []
    for i in range(count):
        o = phoff + i * 0x38
        _, _, off, va, _, filesz, _ = struct.unpack_from(">IIQQQQQ", data, o)
        out.append((va, off, filesz))
    return out


def file_offset(segs, addr):
    for va, off, size in segs:
        if va <= addr < va + size:
            return off + addr - va
    raise ValueError(f"address 0x{addr:x} not backed by file data")


def apply(base_dir, entry):
    src = os.path.normpath(os.path.join(base_dir, entry["source"]))
    dst = os.path.normpath(os.path.join(base_dir, entry["output"]))
    data = bytearray(open(src, "rb").read())
    segs = segments(data)
    applied = []
    for p in entry["patches"]:
        if not p.get("enabled"):
            continue
        for kind, addr, value in p["writes"]:
            off = file_offset(segs, int(addr, 16))
            v = int(value, 16)
            if kind == "byte":
                data[off] = v
            elif kind == "be16":
                struct.pack_into(">H", data, off, v)
            elif kind == "be32":
                struct.pack_into(">I", data, off, v)
            else:
                raise ValueError(kind)
        applied.append(p["name"])
    open(dst, "wb").write(data)
    print(f"{os.path.basename(dst)}: {', '.join(applied) or 'no patches'}")


def main(manifest="recomp/patches.json"):
    base_dir = os.path.dirname(os.path.abspath(manifest))
    for entry in json.load(open(manifest)).values():
        apply(base_dir, entry)


if __name__ == "__main__":
    main(*sys.argv[1:])
