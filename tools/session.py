import argparse
import glob
import os
import re
import statistics
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REC = struct.Struct("<d12f6I")
FIELDS = ["t", "gap", "draw", "tex", "vertices", "indices", "bind", "record", "copy", "complete", "present", "fence", "gpu",
          "draws", "skipped", "uploads", "upload_kb", "pipelines", "copies"]

CENSUS_RE = re.compile(r"^. +([0-9.]+) .*census: process cpu (\d+)% of one core over \d+s:(.*)$", re.M)
THREAD_RE = re.compile(r"\[([^#\]]+)#\d+ (\d+)%u (\d+)%k\]")
AUDIO_RE = re.compile(r"^. +([0-9.]+) .*audio stats: (\d+) blocks, (\d+) late wakeups, (\d+) stale blocks, (\d+) device underruns", re.M)
HITCH_RE = re.compile(r"^. +([0-9.]+) .*hitch: no flip for", re.M)
CRASH_RE = re.compile(r"^E +([0-9.]+) .*crash: (signal .*)$", re.M)
WAIT_RE = re.compile(r"^  \| ([\w-]+) \| ([0-9.]+) \| ([0-9.]+) \|$", re.M)


def latest_session():
    dirs = sorted(glob.glob(os.path.join(ROOT, "run", "sessions", "*")) + glob.glob(os.path.join(ROOT, "out", "sessions", "*")), key=os.path.getmtime)
    return dirs[-1] if dirs else None


def load_frames(d):
    path = os.path.join(d, "frames.bin")
    if not os.path.exists(path):
        return []
    data = open(path, "rb").read()
    if data[:4] != b"GFRM":
        return []
    body = data[8:]
    n = len(body) // REC.size
    return [dict(zip(FIELDS, REC.unpack_from(body, i * REC.size))) for i in range(n)]


def load_log(d):
    path = os.path.join(d, "gowa.log")
    return open(path, encoding="utf-8", errors="replace").read() if os.path.exists(path) else ""


def census(text):
    out = []
    for t, total, rest in CENSUS_RE.findall(text):
        threads = {}
        for name, u, k in THREAD_RE.findall(rest):
            key = name.strip()
            if key.startswith("SPU ") and "BigCell" in key:
                key = "SPU " + key.split()[1]
            threads[key] = threads.get(key, 0) + int(u) + int(k)
        out.append((float(t), int(total), threads))
    return out


def census_at(cen, lo, hi):
    rows = [c for c in cen if lo - 0.5 <= c[0] <= hi + 1.5]
    if not rows:
        return ""
    total = statistics.mean(r[1] for r in rows)
    keys = {}
    for r in rows:
        for k, v in r[2].items():
            keys.setdefault(k, []).append(v)
    spu = [statistics.mean(v) for k, v in keys.items() if k.startswith("SPU ")]
    parts = ["cpu %d%%" % total]
    for k in ("RSX FIFO", "PPU main_thread", "jobsys worker 0"):
        if k in keys:
            parts.append("%s %d%%" % (k.replace("PPU ", ""), statistics.mean(keys[k])))
    if spu:
        parts.append("SPU avg %d%%" % statistics.mean(spu))
    return ", ".join(parts)


def fps_of(frames):
    if len(frames) < 2:
        return 0.0
    span = frames[-1]["t"] - frames[0]["t"]
    return (len(frames) - 1) / span if span > 0 else 0.0


def low_1(frames):
    gaps = sorted((f["gap"] for f in frames), reverse=True)
    if not gaps:
        return 0.0
    k = max(1, len(gaps) // 100)
    return 1000.0 / statistics.mean(gaps[:k])


def episodes(frames, threshold, join_s=1.0):
    eps = []
    for f in frames:
        if f["gap"] < threshold:
            continue
        if eps and f["t"] - eps[-1][-1]["t"] <= join_s:
            eps[-1].append(f)
        else:
            eps.append([f])
    return eps


def mean_of(frames, key):
    return statistics.mean(f[key] for f in frames) if frames else 0.0


def factors(sel, base):
    out = []
    for key, label, unit in (("draw", "RSX draw", "ms"), ("tex", "textures", "ms"), ("vertices", "vertex decode", "ms"), ("copy", "copy engine", "ms"),
                             ("gpu", "GPU", "ms"), ("fence", "GPU wait", "ms"), ("upload_kb", "uploads", "KB"), ("pipelines", "new pipelines", ""), ("draws", "draws", "")):
        a, b = mean_of(sel, key), mean_of(base, key)
        if a > b * 1.3 and a - b > (0.5 if unit == "ms" else 1.0):
            out.append("%s %.1f%s (normal %.1f)" % (label, a, unit, b))
    return out


def critpath_summary(d, lo, hi):
    path = os.path.join(d, "critpath.bin")
    if not os.path.exists(path):
        return ""
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "critpath_report.py"), path, "--from", "%.1f" % lo, "--to", "%.1f" % hi], capture_output=True, text=True)
    block = r.stdout.split("## slow frames")[0]
    m = re.search(r"avg frame ([0-9.]+) ms .*?\nPPU main thread waits: ([0-9.]+) ms/frame", block, re.S)
    waits = [(s, float(ms)) for s, ms, _ in WAIT_RE.findall(block) if s in ("edge-prev", "rsx", "counter", "k9", "graph-spin", "batch-finish", "wws-list")]
    waits.sort(key=lambda w: -w[1])
    comp = re.search(r"\| last completer \(job type\).*?\n\s*\|---.*?\n((?:\s*\|.*\n){1,3})", block)
    tops = []
    if comp:
        for line in comp.group(1).splitlines():
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if len(cells) >= 2:
                tops.append("%s %s" % (cells[0], cells[1]))
    if not m:
        return ""
    return "frame %.1f ms, PPU waits %.1f ms: %s; waiting on %s" % (float(m.group(1)), float(m.group(2)), ", ".join("%s %.1f" % w for w in waits[:4]), ", ".join(tops))


def report(d, args):
    frames = load_frames(d)
    text = load_log(d)
    print("session", d)
    if not frames:
        print("no frames.bin")
        return
    start = frames[0]["t"]
    frames = [f for f in frames if f["t"] - start >= args.skip]
    print("frames %d, %.0f s, avg %.1f fps, 1%% low %.1f fps" % (len(frames), frames[-1]["t"] - frames[0]["t"], fps_of(frames), low_1(frames)))
    print()
    print("timeline (%d s buckets): avg fps / worst frame ms" % args.bucket)
    line = []
    t0 = int(frames[0]["t"] // args.bucket) * args.bucket
    bucket = []
    for f in frames + [None]:
        if f is None or f["t"] >= t0 + args.bucket:
            if bucket:
                line.append("%4d:%4.0f/%-4.0f" % (t0, fps_of(bucket), max(b["gap"] for b in bucket)))
            if f is None:
                break
            while f["t"] >= t0 + args.bucket:
                t0 += args.bucket
            bucket = []
        bucket.append(f)
    for i in range(0, len(line), 8):
        print("  " + "  ".join(line[i:i + 8]))
    cen = census(text)
    base = [f for f in frames if f["gap"] < 25]
    eps = episodes(frames, args.threshold)
    eps.sort(key=lambda e: -sum(f["gap"] for f in e))
    print()
    print("worst episodes (frames over %.0f ms, joined within 1 s), by total time:" % args.threshold)
    for i, e in enumerate(eps[:args.top]):
        lo, hi = e[0]["t"], e[-1]["t"]
        around = [f for f in frames if lo - 0.5 <= f["t"] <= hi + 0.5]
        print("%2d. %6.1f-%6.1f s: %d slow frames, worst %.0f ms, %.1f fps around" % (i + 1, lo, hi, len(e), max(f["gap"] for f in e), fps_of(around)))
        fx = factors(e, base)
        if fx:
            print("      high: " + "; ".join(fx))
        c = census_at(cen, lo, hi)
        if c:
            print("      " + c)
        if args.critpath and i < args.critpath:
            s = critpath_summary(d, lo - 1.0, hi + 1.0)
            if s:
                print("      critpath: " + s)
    audio = [(float(t), int(b), int(l), int(s), int(u)) for t, b, l, s, u in AUDIO_RE.findall(text)]
    glitches = [a for a in audio if a[2] or a[3] or a[4]]
    print()
    if audio:
        print("audio: %d seconds with glitches (late wakeups / stale blocks / device underruns)" % len(glitches))
        for a in glitches[:args.top]:
            print("  %6.1f s: late %d, stale %d, underruns %d" % (a[0], a[2], a[3], a[4]))
    hitches = [float(t) for t in HITCH_RE.findall(text)]
    if hitches:
        print("hitch stack dumps at: " + ", ".join("%.0f" % h for h in hitches[:20]))
    for t, msg in CRASH_RE.findall(text):
        print("CRASH at %s s: %s" % (t, msg))
    warns = {}
    for m in re.finditer(r"^W +[0-9.]+ \[ *\d+\] (\w+): (.{0,70})", text, re.M):
        k = m.group(1) + ": " + re.sub(r"0x[0-9a-f]+|\d+", "#", m.group(2))
        warns[k] = warns.get(k, 0) + 1
    if warns:
        print("warnings:")
        for k, v in sorted(warns.items(), key=lambda kv: -kv[1])[:8]:
            print("  %6d  %s" % (v, k))


def window(d, lo, hi):
    frames = [f for f in load_frames(d) if lo <= f["t"] <= hi]
    print("window %.1f-%.1f s: %d frames, %.1f fps, worst %.0f ms" % (lo, hi, len(frames), fps_of(frames), max((f["gap"] for f in frames), default=0)))
    for key in FIELDS[1:]:
        vals = [f[key] for f in frames]
        if vals:
            print("  %-10s avg %8.2f  p95 %8.2f  max %8.2f" % (key, statistics.mean(vals), sorted(vals)[int(0.95 * (len(vals) - 1))], max(vals)))
    c = census_at(census(load_log(d)), lo, hi)
    if c:
        print("  " + c)
    path = os.path.join(d, "critpath.bin")
    if os.path.exists(path):
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "critpath_report.py"), path, "--from", str(lo), "--to", str(hi)], capture_output=True, text=True)
        print(r.stdout.split("## slow frames")[0])


def main():
    ap = argparse.ArgumentParser(description="analyse a recorded play session (GOWA_SESSION=1)")
    ap.add_argument("dir", nargs="?", help="session directory (default: newest under run/sessions)")
    ap.add_argument("--window", nargs=2, type=float, metavar=("FROM", "TO"), help="detailed report for a time range")
    ap.add_argument("--threshold", type=float, default=50.0, help="slow frame threshold in ms")
    ap.add_argument("--top", type=int, default=10)
    ap.add_argument("--bucket", type=int, default=10)
    ap.add_argument("--skip", type=float, default=0.0, help="ignore the first N seconds")
    ap.add_argument("--critpath", type=int, default=0, help="add a critical-path summary to the N worst episodes")
    args = ap.parse_args()
    d = args.dir or latest_session()
    if not d:
        sys.exit("no session found")
    if args.window:
        window(d, *args.window)
    else:
        report(d, args)


if __name__ == "__main__":
    main()
