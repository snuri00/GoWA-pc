import hashlib
import os
import struct
import sys
from ird import parse

SS = 2048


def walk(iso, lba, size, path, out):
    iso.seek(lba * SS)
    d = iso.read(size)
    i = 0
    while i < len(d):
        n = d[i]
        if n == 0:
            i = (i // SS + 1) * SS
            continue
        r = d[i:i + n]
        i += n
        el = struct.unpack("<I", r[2:6])[0]
        sz = struct.unpack("<I", r[10:14])[0]
        name = r[33:33 + r[32]]
        if name in (b"\0", b"\1"):
            continue
        p = path + "/" + name.decode("latin1").split(";")[0]
        if r[25] & 2:
            walk(iso, el, sz, p, out)
        else:
            out.setdefault(p, []).append((el, sz))


def extract(iso, extents, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, "wb") as f:
        for lba, size in extents:
            iso.seek(lba * SS)
            left = size
            while left:
                b = iso.read(min(left, 1 << 24))
                f.write(b)
                left -= len(b)


def main(iso_path, ird_path, root, fix=False):
    ird = parse(ird_path)
    hashes = dict(ird["files"])
    iso = open(iso_path, "rb")
    iso.seek(16 * SS)
    pvd = iso.read(SS)
    files = {}
    walk(iso, struct.unpack("<I", pvd[158:162])[0], struct.unpack("<I", pvd[166:170])[0], "", files)
    ok = bad = 0
    for p, extents in sorted(files.items()):
        first = extents[0][0]
        total = sum(s for _, s in extents)
        h = hashlib.md5()
        got = 0
        try:
            with open(root + p, "rb") as f:
                while True:
                    b = f.read(1 << 24)
                    if not b:
                        break
                    h.update(b)
                    got += len(b)
        except FileNotFoundError:
            if not fix:
                print("MISSING", p)
                bad += 1
                continue
            extract(iso, extents, root + p)
            print("EXTRACTED", p)
            with open(root + p, "rb") as f:
                while True:
                    b = f.read(1 << 24)
                    if not b:
                        break
                    h.update(b)
                    got += len(b)
        if got == total and hashes.get(first) == h.digest():
            ok += 1
        else:
            print("BAD", p, got, total)
            bad += 1
    print("ok", ok, "bad", bad, "ird_files", len(hashes))


if __name__ == "__main__":
    main(*sys.argv[1:4], fix="--fix" in sys.argv)
