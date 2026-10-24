import argparse
import glob
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, "recomp", "spu", "block_manifest.txt")
FW_SOURCES = ("libsre", "libsmvd2", "libvdec", "libsync2", "libfiber", "libaudio", "libsysutil", "libio", "libnet", "libgcm_sys", "libfs", "liblv2", "libsysmodule")


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
            i += 2 + n
            blocks[(lsa, tuple(raw))] = True
    return sorted(blocks.keys())


def load_sources(elf, fw_dir):
    srcs = [("gowa", open(elf, "rb").read())]
    for name in FW_SOURCES:
        path = os.path.join(fw_dir, name + ".prx")
        if os.path.exists(path):
            srcs.append((name, open(path, "rb").read()))
    return srcs


def arith(raw):
    if len(raw) < 3:
        return None
    w = [struct.unpack(">I", struct.pack("<I", r))[0] for r in raw]
    step = (w[1] - w[0]) & 0xFFFFFFFF
    for k in range(1, len(w) - 1):
        if (w[k] - w[k - 1]) & 0xFFFFFFFF != step:
            return None
    return w[0], step, w[-1]


def make(args):
    paths = sorted(glob.glob(os.path.join(args.blocks, "run*.bin")))
    if not paths:
        raise SystemExit("no run*.bin in %s" % args.blocks)
    blocks = load_blocks(paths)
    srcs = load_sources(args.elf, args.fw)
    lines = []
    miss = 0
    for lsa, raw in blocks:
        b = struct.pack("<%dI" % len(raw), *raw)
        hit = None
        for name, data in srcs:
            off = data.find(b)
            if off >= 0:
                hit = "%05x %x %s %x" % (lsa, len(raw), name, off)
                break
        if hit is None:
            a = arith(raw)
            if a is not None:
                hit = "%05x %x seq %08x %08x %08x" % (lsa, len(raw), a[0], a[1], a[2])
        if hit is None:
            miss += 1
            print("unlocated block %05x (%d words)" % (lsa, len(raw)), file=sys.stderr)
            continue
        lines.append(hit)
    open(args.manifest, "w", newline="\n").write("\n".join(lines) + "\n")
    print("%d blocks, %d unlocated" % (len(lines), miss))


def build(manifest, elf, fw_dir, out_path):
    srcs = dict(load_sources(elf, fw_dir))
    out = bytearray()
    n_ok = 0
    for line in open(manifest):
        t = line.split()
        if not t:
            continue
        lsa, n = int(t[0], 16), int(t[1], 16)
        if t[2] == "seq":
            base, step, tail = int(t[3], 16), int(t[4], 16), int(t[5], 16)
            words = [(base + k * step) & 0xFFFFFFFF for k in range(n - 1)] + [tail]
            b = struct.pack(">%dI" % n, *words)
        else:
            data = srcs.get(t[2])
            if data is None:
                raise SystemExit("missing source %s for block %05x (firmware dir %s)" % (t[2], lsa, fw_dir))
            off = int(t[3], 16)
            b = data[off: off + 4 * n]
            if len(b) != 4 * n:
                raise SystemExit("block %05x out of range in %s" % (lsa, t[2]))
        out += struct.pack("<II", lsa, n) + b
        n_ok += 1
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    open(out_path, "wb").write(bytes(out))
    return n_ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=("make", "build"))
    ap.add_argument("--elf", default=os.path.join(ROOT, "bin", "01.12", "GOWA.patched.elf"))
    ap.add_argument("--fw", default=os.path.join(ROOT, "fw", "4.91"))
    ap.add_argument("--blocks", default=os.path.join(ROOT, "recomp", "spu", "blocks"))
    ap.add_argument("--manifest", default=MANIFEST)
    ap.add_argument("--out", default=os.path.join(ROOT, "out", "gen", "blocks", "run_manifest.bin"))
    args = ap.parse_args()
    if args.mode == "make":
        make(args)
    else:
        print("%d blocks" % build(args.manifest, args.elf, args.fw, args.out))


if __name__ == "__main__":
    main()
