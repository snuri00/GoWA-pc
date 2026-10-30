import argparse
import datetime
import json
import os
import re
import statistics
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNS = os.path.join(ROOT, "out", "runs")
OUT = os.path.join(ROOT, "out", "abtest")

FLIP_RE = re.compile(r"^. +([0-9.]+) .*rsx: flip (\d+) .*worst ([0-9.]+) ms", re.M)
DRAWS_RE = re.compile(r"^. +([0-9.]+) .*rsx: renderer: (\d+) draws", re.M)
GPU_RE = re.compile(r"^. +([0-9.]+) .*gpu: fence wait ([0-9.]+) ms(?:, gpu ([0-9.]+) ms)?", re.M)
REC_RE = re.compile(r"^. +([0-9.]+) .*rsx: renderer per frame: draw ([0-9.]+) ms", re.M)
FIFO_RE = re.compile(r"^. +([0-9.]+) .*rsx: fifo 5s: busy ([0-9.]+)%, idle [0-9.]+%, ([0-9.]+) idle sleeps/s", re.M)
CENSUS_RE = re.compile(r"^. +([0-9.]+) .*census: process cpu (\d+)%.*?\[RSX FIFO#\d+ (\d+)%u (\d+)%k\]", re.M)


def parse_cfg(text):
    name, _, envs = text.partition(":")
    env = [e for e in envs.split(",") if e] if envs else []
    return name, env


def window(points, lo, hi):
    return [p for p in points if lo <= p[0] <= hi]


def mean(v):
    return sum(v) / len(v) if v else 0.0


def analyze(log, lo, hi):
    text = open(log, encoding="utf-8", errors="replace").read()
    flips = [(float(t), int(n), float(w)) for t, n, w in FLIP_RE.findall(text)]
    fw = window(flips, lo, hi)
    m = {"fps": 0.0, "worst": 0.0, "worst_p90": 0.0, "draws_per_frame": 0.0}
    if len(fw) >= 2 and fw[-1][0] > fw[0][0]:
        m["fps"] = (fw[-1][1] - fw[0][1]) / (fw[-1][0] - fw[0][0])
        ws = sorted(f[2] for f in fw[1:])
        m["worst"] = ws[-1]
        m["worst_p90"] = ws[int(0.9 * (len(ws) - 1))]
    draws = window([(float(t), int(n)) for t, n in DRAWS_RE.findall(text)], lo, hi)
    if len(draws) >= 2 and len(fw) >= 2:
        frames = [f for f in flips if draws[0][0] <= f[0] <= draws[-1][0]]
        if len(frames) >= 2 and frames[-1][1] > frames[0][1]:
            m["draws_per_frame"] = (draws[-1][1] - draws[0][1]) / (frames[-1][1] - frames[0][1])
    gpu = window([(float(t), float(f), float(g or 0)) for t, f, g in GPU_RE.findall(text)], lo, hi)
    m["fence_ms"] = mean([g[1] for g in gpu])
    m["gpu_ms"] = mean([g[2] for g in gpu])
    m["record_ms"] = mean([float(d) for t, d in REC_RE.findall(text) if lo <= float(t) <= hi])
    fifo = [(float(t), float(b), float(n)) for t, b, n in FIFO_RE.findall(text) if lo <= float(t) <= hi]
    if fifo:
        m["fifo_busy"] = mean([f[1] for f in fifo])
        m["fifo_sleeps"] = mean([f[2] for f in fifo])
    cen = [(float(t), int(c), int(u) + int(k)) for t, c, u, k in CENSUS_RE.findall(text) if lo <= float(t) <= hi]
    if cen:
        m["cpu_pct"] = mean([c[1] for c in cen])
        m["fifo_pct"] = mean([c[2] for c in cen])
    return m


def critpath(run_dir, lo, hi):
    path = os.path.join(run_dir, "critpath.bin")
    if not os.path.exists(path):
        return {}
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "critpath_report.py"), run_dir, "--from", str(lo), "--to", str(hi)], capture_output=True, text=True)
    text = r.stdout
    out = {}
    m = re.search(r"## all frames: \d+ frames, avg frame ([0-9.]+) ms.*?\nPPU main thread waits: ([0-9.]+) ms/frame", text, re.S)
    if m:
        out["frame_ms"] = float(m.group(1))
        out["ppu_wait_ms"] = float(m.group(2))
    block = text.split("## slow frames")[0]
    for site, ms in re.findall(r"^  \| ([\w-]+) \| ([0-9.]+) \| [0-9.]+ \|$", block, re.M):
        if site in ("edge-prev", "rsx", "counter", "k9", "graph-spin", "batch-finish", "wws-list", "batch-addjob"):
            out["wait_" + site] = float(ms)
    return out


def run_one(scenario, name, env, secs, crit, census):
    cmd = [sys.executable, os.path.join(ROOT, "tools", "testrun.py"), "run", scenario, "--name", name, "--secs", str(secs), "--snap-every", "0"]
    envs = list(env)
    if crit:
        envs.append("GOWA_CRITPATH=1")
    if census:
        envs.append("GOWA_THREAD_CPU=5")
    for e in envs:
        cmd += ["--env", e]
    r = subprocess.run(cmd, capture_output=True, text=True)
    m = re.search(r"^run (\S+) for", r.stdout, re.M)
    return os.path.join(RUNS, m.group(1)) if m else None


def fmt(v):
    return "%.1f" % v if isinstance(v, float) else str(v)


def main():
    ap = argparse.ArgumentParser(description="interleaved A/B runs of a testrun scenario")
    ap.add_argument("--cfg", action="append", required=True, help="name[:ENV=V,ENV2=V]; the first one is the reference")
    ap.add_argument("--scenario", default="fight")
    ap.add_argument("--reps", type=int, default=2)
    ap.add_argument("--secs", type=int, default=130)
    ap.add_argument("--window", default="50:125", help="seconds measured (log time), lo:hi")
    ap.add_argument("--critpath", action="store_true", help="record and summarize the PPU critical path")
    ap.add_argument("--census", action="store_true", help="log per-thread CPU (RSX FIFO load)")
    ap.add_argument("--tag", default="ab")
    args = ap.parse_args()
    lo, hi = (float(x) for x in args.window.split(":"))
    cfgs = [parse_cfg(c) for c in args.cfg]
    results = {name: [] for name, _ in cfgs}
    for rep in range(args.reps):
        for name, env in cfgs:
            d = run_one(args.scenario, "%s-%s-%d" % (args.tag, name, rep), env, args.secs, args.critpath, args.census)
            if not d or not os.path.exists(os.path.join(d, "gowa.log")):
                print("%-12s rep %d: run failed" % (name, rep), flush=True)
                continue
            m = analyze(os.path.join(d, "gowa.log"), lo, hi)
            if args.critpath:
                m.update(critpath(d, lo, hi))
            m["run"] = os.path.basename(d)
            results[name].append(m)
            print("%-12s rep %d: fps %.1f worst %.0f ms, %.0f draws/frame" % (name, rep, m["fps"], m["worst"], m["draws_per_frame"]), flush=True)
    every = [m["draws_per_frame"] for runs in results.values() for m in runs if m["draws_per_frame"] > 0]
    ref_draws = statistics.median(every) if every else 0
    for runs in results.values():
        for m in runs:
            m["valid"] = bool(ref_draws) and m["draws_per_frame"] >= 0.6 * ref_draws and m["fps"] > 0
    keys = ["fps", "worst", "worst_p90", "draws_per_frame", "fence_ms", "gpu_ms", "record_ms", "cpu_pct", "fifo_pct", "fifo_busy", "fifo_sleeps", "frame_ms", "ppu_wait_ms"]
    keys += sorted({k for runs in results.values() for m in runs for k in m if k.startswith("wait_")})
    summary = {}
    for name, runs in results.items():
        good = [m for m in runs if m["valid"]]
        s = {"runs": len(runs), "valid": len(good)}
        for k in keys:
            vals = [m[k] for m in good if k in m]
            if vals:
                s[k] = mean(vals)
                if k == "fps" and len(vals) > 1:
                    s["fps_sd"] = statistics.stdev(vals)
        summary[name] = s
    ref = summary[cfgs[0][0]]
    print()
    print("window %s s, %d reps, scenario %s; invalid = draws/frame < 60%% of median %.0f (scenario diverged)" % (args.window, args.reps, args.scenario, ref_draws))
    cols = [k for k in keys if any(k in s for s in summary.values())]
    print("%-12s %5s  %s" % ("config", "valid", "  ".join("%9s" % c[:9] for c in cols)))
    for name, s in summary.items():
        print("%-12s %2d/%-2d  %s" % (name, s["valid"], s["runs"], "  ".join("%9s" % (fmt(s[c]) if c in s else "-") for c in cols)))
        if name != cfgs[0][0] and "fps" in s and "fps" in ref and ref["fps"]:
            print("%-12s        fps %+.1f%% vs %s (sd %.1f / %.1f)" % ("", 100.0 * (s["fps"] / ref["fps"] - 1), cfgs[0][0], s.get("fps_sd", 0), ref.get("fps_sd", 0)))
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, datetime.datetime.now().strftime("%Y%m%d-%H%M%S") + "-" + args.tag + ".json")
    commit = subprocess.run(["git", "-C", ROOT, "log", "--oneline", "-1"], capture_output=True, text=True).stdout.strip()
    json.dump({"args": vars(args), "commit": commit, "summary": summary, "runs": results}, open(path, "w"), indent=1)
    print("saved", path)


if __name__ == "__main__":
    main()
