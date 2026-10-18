import argparse
import glob
import os
import struct

import numpy as np

RESTART = 0xFFFFFFFF


def load(path):
    b = open(path, "rb").read()
    if len(b) < 96:
        return None
    magic, draw, prim, vcount, icount, stride, target, cmask = struct.unpack_from("<8I", b, 0)
    m = np.frombuffer(b, "<f4", 16, 32).reshape(4, 4)
    off = 32 + 64
    nv = vcount if stride else 1
    pos = np.frombuffer(b, "<f4", nv * 4, off).reshape(nv, 4)
    off += nv * 16
    idx = np.frombuffer(b, "<u4", icount, off)
    return dict(draw=draw, prim=prim, vcount=vcount, target=target, cmask=cmask, m=m, pos=pos, idx=idx)


def triangles(prim, idx):
    out = []
    if prim == 5:
        for i in range(0, len(idx) - 2, 3):
            out.append((idx[i], idx[i + 1], idx[i + 2]))
    elif prim == 6:
        k = 0
        for i in range(len(idx) - 2):
            a, b, c = idx[i], idx[i + 1], idx[i + 2]
            if RESTART in (a, b, c):
                k = 0
                continue
            out.append((a, b, c) if k % 2 == 0 else (b, a, c))
            k += 1
    elif prim == 7:
        for i in range(1, len(idx) - 1):
            out.append((idx[0], idx[i], idx[i + 1]))
    elif prim == 8:
        for i in range(0, len(idx) - 3, 4):
            out.append((idx[i], idx[i + 1], idx[i + 2]))
            out.append((idx[i], idx[i + 2], idx[i + 3]))
    return out


def main():
    ap = argparse.ArgumentParser(description="find draws covering a screen pixel from GOWA_DUMP_GEOM dumps")
    ap.add_argument("dir")
    ap.add_argument("x", type=float)
    ap.add_argument("y", type=float)
    ap.add_argument("--w", type=float, default=1280)
    ap.add_argument("--h", type=float, default=720)
    args = ap.parse_args()
    hits = []
    for f in sorted(glob.glob(os.path.join(args.dir, "geom_*.bin"))):
        d = load(f)
        if d is None or d["pos"].shape[0] < 3:
            continue
        clip = d["pos"] @ d["m"]
        w = clip[:, 3]
        with np.errstate(divide="ignore", invalid="ignore"):
            sx = (clip[:, 0] / w * 0.5 + 0.5) * args.w
            sy = (0.5 - clip[:, 1] / w * 0.5) * args.h
            sz = clip[:, 2] / w
        best = None
        for a, b, c in triangles(d["prim"], d["idx"]):
            if max(a, b, c) >= len(w) or min(w[a], w[b], w[c]) <= 0:
                continue
            x0, y0, x1, y1, x2, y2 = sx[a], sy[a], sx[b], sy[b], sx[c], sy[c]
            den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
            if abs(den) < 1e-9:
                continue
            l0 = ((y1 - y2) * (args.x - x2) + (x2 - x1) * (args.y - y2)) / den
            l1 = ((y2 - y0) * (args.x - x2) + (x0 - x2) * (args.y - y2)) / den
            l2 = 1 - l0 - l1
            if l0 >= 0 and l1 >= 0 and l2 >= 0:
                z = l0 * sz[a] + l1 * sz[b] + l2 * sz[c]
                ww = l0 * w[a] + l1 * w[b] + l2 * w[c]
                if best is None or z < best[0]:
                    best = (z, ww, (a, b, c))
        if best:
            hits.append((best[0], d["draw"], d["target"], d["cmask"], best[1], d["vcount"], best[2]))
    hits.sort()
    for z, draw, target, cmask, ww, vc, tri in hits[:30]:
        print("draw #%4d target 0x%07x cmask 0x%08x depth %.5f w %.3f verts %d tri %s" % (draw, target, cmask, z, ww, vc, tri))


if __name__ == "__main__":
    main()
