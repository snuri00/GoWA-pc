import argparse
import glob
import os
import subprocess
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(skip, args):
    cmd = [sys.executable, os.path.join(ROOT, "tools", "testrun.py"), "run", args.scenario, "--secs", str(args.secs), "--snap-from", str(args.at), "--snap-every", "100",
           "--note", "bisect %s" % skip]
    if skip:
        cmd += ["--env", "GOWA_SKIP_DRAWS=%s" % skip]
    for e in args.env or []:
        cmd += ["--env", e]
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    runs = sorted(glob.glob(os.path.join(ROOT, "out", "runs", "*-" + args.scenario)))
    shots = sorted(glob.glob(os.path.join(runs[-1], "snap_*.jpg")))
    if not shots:
        return None
    im = Image.open(shots[0]).convert("RGB")
    if args.metric == "brownband":
        w, h = im.size
        px = im.load()
        cols = 0
        for x in range(0, w, 2):
            n = 0
            for y in range(0, h, 4):
                r, g, b = px[x, y]
                if r > 25 and r > 1.15 * g and r > 1.6 * b:
                    n += 1
            if n >= 0.8 * (h // 4):
                cols += 1
        return (cols,)
    return im.getpixel((args.x, args.y))


def dist(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5


def main():
    ap = argparse.ArgumentParser(description="bisect which draw in a frame produces a pixel")
    ap.add_argument("scenario")
    ap.add_argument("--at", type=float, required=True)
    ap.add_argument("--secs", type=int, required=True)
    ap.add_argument("--x", type=int, default=0)
    ap.add_argument("--y", type=int, default=0)
    ap.add_argument("--metric", default="pixel")
    ap.add_argument("--lo", type=int, default=0)
    ap.add_argument("--hi", type=int, required=True)
    ap.add_argument("--tol", type=float, default=40.0)
    ap.add_argument("--env", action="append")
    args = ap.parse_args()
    base = run(None, args)
    print("baseline pixel", base, flush=True)
    lo, hi = args.lo, args.hi
    while lo < hi:
        mid = (lo + hi) // 2
        px = run("%d-%d" % (lo, mid), args)
        if args.metric == "brownband":
            gone = px is not None and px[0] < base[0] * 0.4
        else:
            gone = px is not None and dist(px, base) > args.tol
        print("skip %d-%d -> %s %s" % (lo, mid, px, "changed" if gone else "same"), flush=True)
        if gone:
            hi = mid
        else:
            lo = mid + 1
    print("draw", lo)


if __name__ == "__main__":
    main()
