import hashlib
import os
import struct
import sys
from ird import parse

SS = 2048


def root_record(iso):
    for n in range(16, 32):
        iso.seek(n * SS)
        vd = iso.read(SS)
        if vd[0] == 255:
            break
        if vd[0] == 2 and vd[88:91] in (b"%/@", b"%/C", b"%/E"):
            return vd[156:190], "utf-16-be"
    iso.seek(16 * SS)
    return iso.read(SS)[156:190], "latin1"


def walk(iso, lba, size, path, out, enc):
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
        p = path + "/" + name.decode(enc).split(";")[0]
        if r[25] & 2:
            walk(iso, el, sz, p, out, enc)
        else:
            out.setdefault(p, []).append((el, sz))


def list_files(iso):
    rec, enc = root_record(iso)
    files = {}
    walk(iso, struct.unpack("<I", rec[2:6])[0], struct.unpack("<I", rec[10:14])[0], "", files, enc)
    return files


def host_path(dst_root, p):
    return dst_root + p.rstrip(".")


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


def md5_file(path):
    h = hashlib.md5()
    got = 0
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 24), b""):
            h.update(b)
            got += len(b)
    return h.digest(), got


def main(iso_path, ird_path, dst_root, fix=False):
    hashes = dict(parse(ird_path)["files"])
    iso = open(iso_path, "rb")
    ok = bad = 0
    for p, extents in sorted(list_files(iso).items()):
        dst = host_path(dst_root, p)
        if not os.path.exists(dst):
            if not fix:
                print("MISSING", p)
                bad += 1
                continue
            extract(iso, extents, dst)
            print("EXTRACTED", p)
        digest, got = md5_file(dst)
        if got == sum(s for _, s in extents) and hashes.get(extents[0][0]) == digest:
            ok += 1
        else:
            print("BAD", p, got)
            bad += 1
    print("ok", ok, "bad", bad, "ird_files", len(hashes))


if __name__ == "__main__":
    main(*sys.argv[1:4], fix="--fix" in sys.argv)
