import argparse
import datetime
import json
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN_DIR = os.path.join(ROOT, "run")
RUNS = os.path.join(ROOT, "out", "runs")
EXE = os.environ.get("GOWA_TESTRUN_EXE") or (os.path.join(ROOT, "build", "God of War Ascension.exe") if sys.platform == "win32" else os.path.join(ROOT, "build-linux", "God of War Ascension"))
KEEP = 15


def pad_menu(duration):
    return [("cross", t) for t in range(10, 46, 3)]


def pad_continue(duration):
    ev = [("cross", t) for t in range(10, 70, 3)]
    buttons = ["square", "square", "triangle", "cross"]
    ev += [(buttons[(t // 2) % 4], t) for t in range(100, duration - 4, 2)]
    sticks = ["rx+", "rx-", "ry+", "ry-"]
    ev += [(sticks[(t // 4) % 4], "%d:1.0" % (t + 2)) for t in range(150, duration - 4, 4)]
    return ev


def pad_ingame(duration):
    ev = [("cross", t) for t in range(10, 27, 4)]
    for t in range(30, 91, 5):
        ev += [("start", t), ("cross", t + 2)]
    ev += [("start", t) for t in range(95, 151, 6)]
    ev += [("circle", 153), ("circle", 155)]
    buttons = ["square", "triangle", "cross"]
    ev += [(buttons[(t // 3) % 3], t) for t in range(156, duration - 4, 3)]
    return ev


def pad_idle(duration):
    return [("cross", t) for t in range(10, 70, 3)]


def pad_boot(duration):
    return []


SCENARIOS = {
    "boot": (60, pad_boot, 2.0, 5.0),
    "menu": (95, pad_menu, 5.0, 30.0),
    "ingame": (480, pad_ingame, 20.0, 150.0),
    "continue": (240, pad_continue, 10.0, 80.0),
    "idle": (600, pad_idle, 30.0, 80.0),
    "fight": (130, pad_continue, 10.0, 50.0),
}

SCENARIO_ENV = {
    "fight": {"GOWA_TURBO": "3@20"},
}


def run_dirs():
    if not os.path.isdir(RUNS):
        return []
    return sorted(d for d in os.listdir(RUNS) if os.path.isdir(os.path.join(RUNS, d)))


def resolve(name):
    dirs = run_dirs()
    if name in ("last", "-1"):
        return os.path.join(RUNS, dirs[-1])
    if name == "-2":
        return os.path.join(RUNS, dirs[-2])
    matches = [d for d in dirs if name in d]
    if not matches:
        sys.exit("no run matches " + name)
    return os.path.join(RUNS, matches[-1])


def last_flip(log_path, state):
    try:
        with open(log_path, "rb") as f:
            f.seek(state.get("pos", 0))
            data = f.read()
            state["pos"] = f.tell()
    except OSError:
        return state.get("flip")
    for m in re.finditer(rb"I +([0-9.]+) [^\r\n]*rsx: flip", data):
        state["flip"] = float(m.group(1))
    return state.get("flip")


def supervise(env, duration):
    proc = subprocess.Popen([EXE], cwd=RUN_DIR, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t0 = time.time()
    state = {}
    requested = False
    log_path = os.path.join(RUN_DIR, "gowa.log")
    while True:
        try:
            return proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            pass
        now = time.time() - t0
        if now >= duration:
            break
        flip = last_flip(log_path, state)
        if not requested and flip is not None and now > 40 and now - flip > 45:
            requested = True
            open(os.path.join(RUN_DIR, "blackbox.req"), "w").close()
            print("no flips for %.0fs, requesting blackbox capture" % (now - flip))
    proc.kill()
    if sys.platform == "win32":
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)], capture_output=True)
    else:
        proc.wait()
    return 124


def keep_blackbox(out):
    src = os.path.join(RUN_DIR, "blackbox")
    log = os.path.join(out, "gowa.log")
    has = os.path.exists(log) and b"blackbox: capture" in open(log, "rb").read()
    if not has:
        return
    dst = os.path.join(ROOT, "out", "blackbox", os.path.basename(out))
    os.makedirs(dst, exist_ok=True)
    shutil.copy(log, os.path.join(dst, "gowa.log"))
    if os.path.isdir(src):
        for f in os.listdir(src):
            shutil.move(os.path.join(src, f), os.path.join(dst, f))
    boxes = sorted(os.listdir(os.path.join(ROOT, "out", "blackbox")))
    for d in boxes[:-4]:
        shutil.rmtree(os.path.join(ROOT, "out", "blackbox", d), ignore_errors=True)
    print("blackbox kept in", dst)


def prune():
    dirs = [d for d in run_dirs() if not os.path.exists(os.path.join(RUNS, d, "keep"))]
    for d in dirs[:-KEEP]:
        shutil.rmtree(os.path.join(RUNS, d), ignore_errors=True)


def convert_snaps(out):
    from PIL import Image
    shots = []
    for f in sorted(os.listdir(out)):
        if f.startswith("snap_") and f.endswith(".ppm"):
            src = os.path.join(out, f)
            try:
                Image.open(src).convert("RGB").save(src[:-4] + ".jpg", quality=88)
                os.remove(src)
                shots.append(f[:-4] + ".jpg")
            except Exception:
                pass
    return shots


def summarize(out, duration, rc, wall):
    log = os.path.join(out, "gowa.log")
    text = open(log, encoding="utf-8", errors="replace").read() if os.path.exists(log) else ""
    flips = [(float(m.group(1)), int(m.group(2))) for m in re.finditer(r"^. +([0-9.]+) .*rsx: flip (\d+)", text, re.M)]
    s = {"duration": duration, "exit_code": rc, "wall_seconds": round(wall, 1)}
    s["last_flip_time"] = flips[-1][0] if flips else 0
    s["flips"] = flips[-1][1] if flips else 0
    gap = max(flips[-1][0] - flips[-2][0], 1.0) if len(flips) >= 2 else 15.0
    s["hang"] = bool(flips) and flips[-1][0] < duration - max(15.0, 3 * gap + 5) and rc != 0
    s["window_closed"] = "window closed" in text
    rates = {}
    for a in range(0, duration, 30):
        win = [f for f in flips if a <= f[0] < a + 30]
        if len(win) >= 2 and win[-1][0] > win[0][0]:
            rates[str(a)] = round((win[-1][1] - win[0][1]) / (win[-1][0] - win[0][0]), 2)
    s["flips_per_sec"] = rates
    s["crash"] = len(re.findall(r"crash: exception", text))
    s["errors"] = len(re.findall(r"^E ", text, re.M))
    s["warnings"] = len(re.findall(r"^W ", text, re.M))
    s["spu_mismatch"] = text.count("MISMATCH")
    s["vk_failures"] = len(re.findall(r"gpu: vk\w+ failed", text))
    s["texture_copies"] = len(re.findall(r"rsxcopy: nv3089", text))
    s["savedata_writes"] = len(re.findall(r"savedata: write", text))
    errs = {}
    for m in re.finditer(r"^[EW] +[0-9.]+ \[ *\d+\] (\w+): (.{0,80})", text, re.M):
        key = m.group(1) + ": " + re.sub(r"0x[0-9a-f]+|\d+", "#", m.group(2))
        errs[key] = errs.get(key, 0) + 1
    s["top_messages"] = sorted(errs.items(), key=lambda kv: -kv[1])[:12]
    return s


def cmd_run(args):
    duration, padfn, snap_every, snap_from = SCENARIOS[args.scenario]
    duration = args.secs or duration
    snap_every = args.snap_every if args.snap_every is not None else snap_every
    snap_from = args.snap_from if args.snap_from is not None else snap_from
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    name = stamp + "-" + (args.name or args.scenario)
    out = os.path.join(RUNS, name)
    os.makedirs(out, exist_ok=True)
    env = dict(os.environ)
    env["GOWA_ROOT"] = ROOT
    if "GOWA_RPCS3_DIR" not in env and os.path.isdir(os.path.join(RUN_DIR, "rpcs3")):
        env["GOWA_RPCS3_DIR"] = os.path.join(RUN_DIR, "rpcs3")
    env["GOWA_SNAP_EVERY"] = str(snap_every if snap_every > 0 else 1e9)
    env["GOWA_SNAP_FROM"] = str(snap_from)
    env["GOWA_SNAP_DIR"] = out
    for k, v in SCENARIO_ENV.get(args.scenario, {}).items():
        env.setdefault(k, v)
    pad = padfn(duration if args.scenario != "fight" else 240)
    if pad:
        env["GOWA_AUTOPAD"] = ",".join("%s@%s" % (b, t if isinstance(t, str) else "%d:0.6" % t) for b, t in pad)
    extra = {}
    for kv in args.env or []:
        k, _, v = kv.partition("=")
        env[k] = v
        extra[k] = v
    if args.trace:
        env["GOWA_EVTRACE"] = args.trace
        env["GOWA_EVTRACE_FILE"] = os.path.join(out, "trace.bin")
    meta = {"scenario": args.scenario, "duration": duration, "env": extra, "trace": args.trace or "", "note": args.note or ""}
    try:
        meta["commit"] = subprocess.run(["git", "-C", ROOT, "log", "--oneline", "-1"], capture_output=True, text=True).stdout.strip()
        meta["dirty"] = bool(subprocess.run(["git", "-C", ROOT, "status", "--porcelain", "runtime", "recomp"], capture_output=True, text=True).stdout.strip())
    except Exception:
        pass
    json.dump(meta, open(os.path.join(out, "meta.json"), "w"), indent=1)
    print("run", name, "for", duration, "s")
    golden = args.save or os.path.join(RUN_DIR, "savedata_golden")
    if os.path.isdir(golden) and not args.keep_save:
        dst = os.path.join(RUN_DIR, "vfs", "dev_hdd0", "home", "00000001", "savedata")
        for d in os.listdir(golden):
            shutil.rmtree(os.path.join(dst, d), ignore_errors=True)
            shutil.copytree(os.path.join(golden, d), os.path.join(dst, d))
    for f in ("blackbox.req", "blackbox.done"):
        if os.path.exists(os.path.join(RUN_DIR, f)):
            os.remove(os.path.join(RUN_DIR, f))
    shutil.rmtree(os.path.join(RUN_DIR, "blackbox"), ignore_errors=True)
    t0 = time.time()
    rc = supervise(env, duration)
    wall = time.time() - t0
    time.sleep(1)
    if os.path.exists(os.path.join(RUN_DIR, "gowa.log")):
        shutil.copy(os.path.join(RUN_DIR, "gowa.log"), os.path.join(out, "gowa.log"))
    if env.get("GOWA_CRITPATH", "0") not in ("", "0") and os.path.exists(os.path.join(RUN_DIR, "critpath.bin")):
        shutil.move(os.path.join(RUN_DIR, "critpath.bin"), os.path.join(out, "critpath.bin"))
    keep_blackbox(out)
    shots = convert_snaps(out)
    summary = summarize(out, duration, rc, wall)
    summary["snapshots"] = len(shots)
    json.dump(summary, open(os.path.join(out, "summary.json"), "w"), indent=1)
    make_sheet(out, shots, os.path.join(out, "sheet.jpg"))
    prune()
    print_summary(name, summary)


def print_summary(name, s):
    print("==", name)
    for k in ("exit_code", "hang", "window_closed", "flips", "last_flip_time", "crash", "errors", "warnings", "spu_mismatch", "vk_failures", "texture_copies", "savedata_writes", "snapshots"):
        if k in s:
            print("  %-16s %s" % (k, s[k]))
    print("  flips/s by 30s:", " ".join("%s:%s" % kv for kv in s.get("flips_per_sec", {}).items()))
    for msg, n in s.get("top_messages", [])[:8]:
        print("  %5d  %s" % (n, msg))


def snap_time(f):
    m = re.match(r"snap_([0-9.]+)\.jpg", f)
    return float(m.group(1)) if m else None


def make_sheet(out, shots, dest, cols=4, tw=400):
    from PIL import Image, ImageDraw
    shots = [f for f in shots if snap_time(f) is not None]
    if not shots:
        return
    first = Image.open(os.path.join(out, shots[0]))
    th = int(tw * first.height / first.width)
    rows = (len(shots) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * tw, rows * (th + 16)), (60, 60, 60))
    d = ImageDraw.Draw(sheet)
    for i, f in enumerate(shots):
        im = Image.open(os.path.join(out, f)).convert("RGB").resize((tw, th))
        x, y = (i % cols) * tw, (i // cols) * (th + 16)
        sheet.paste(im, (x, y + 16))
        d.text((x + 3, y + 2), "%.0fs" % snap_time(f), fill=(255, 255, 0))
    sheet.save(dest, quality=85)


def cmd_show(args):
    out = resolve(args.run)
    s = json.load(open(os.path.join(out, "summary.json")))
    print_summary(os.path.basename(out), s)
    print("sheet:", os.path.join(out, "sheet.jpg"))


def cmd_list(args):
    for d in run_dirs():
        p = os.path.join(RUNS, d, "summary.json")
        if os.path.exists(p):
            s = json.load(open(p))
            m = json.load(open(os.path.join(RUNS, d, "meta.json")))
            r = list(s.get("flips_per_sec", {}).values())
            print("%-34s flips %5d last %6.1fs hang %-5s crash %d err %d avg %.1f/s  %s" % (d, s["flips"], s["last_flip_time"], s["hang"], s["crash"], s["errors"],
                                                                                          (sum(r) / len(r)) if r else 0, m.get("note", "")))


def cmd_compare(args):
    from PIL import Image, ImageDraw
    a, b = resolve(args.a), resolve(args.b)
    sa = json.load(open(os.path.join(a, "summary.json")))
    sb = json.load(open(os.path.join(b, "summary.json")))
    print("%-16s %-20s %-20s" % ("", os.path.basename(a)[:20], os.path.basename(b)[:20]))
    for k in ("flips", "last_flip_time", "hang", "crash", "errors", "warnings", "spu_mismatch", "vk_failures", "texture_copies", "savedata_writes"):
        print("%-16s %-20s %-20s" % (k, sa.get(k), sb.get(k)))
    keys = sorted(set(sa.get("flips_per_sec", {})) | set(sb.get("flips_per_sec", {})), key=int)
    print("flips/s:", " ".join("%s:%s/%s" % (k, sa["flips_per_sec"].get(k, "-"), sb["flips_per_sec"].get(k, "-")) for k in keys))
    ta = {snap_time(f): f for f in os.listdir(a) if snap_time(f) is not None}
    tb = {snap_time(f): f for f in os.listdir(b) if snap_time(f) is not None}
    pairs = []
    for t in sorted(ta):
        if not tb:
            break
        nb = min(tb, key=lambda x: abs(x - t))
        if abs(nb - t) <= 6:
            pairs.append((t, ta[t], tb[nb]))
    pairs = pairs[:: max(1, len(pairs) // args.max)][: args.max]
    if not pairs:
        print("no matching snapshots")
        return
    tw = 480
    first = Image.open(os.path.join(a, pairs[0][1]))
    th = int(tw * first.height / first.width)
    sheet = Image.new("RGB", (2 * tw, len(pairs) * (th + 16)), (60, 60, 60))
    d = ImageDraw.Draw(sheet)
    for i, (t, fa, fb) in enumerate(pairs):
        y = i * (th + 16)
        sheet.paste(Image.open(os.path.join(a, fa)).convert("RGB").resize((tw, th)), (0, y + 16))
        sheet.paste(Image.open(os.path.join(b, fb)).convert("RGB").resize((tw, th)), (tw, y + 16))
        d.text((3, y + 2), "%.0fs  %s" % (t, os.path.basename(a)), fill=(255, 255, 0))
        d.text((tw + 3, y + 2), os.path.basename(b), fill=(255, 255, 0))
    dest = os.path.join(RUNS, "compare_%s_vs_%s.jpg" % (os.path.basename(a)[:15], os.path.basename(b)[:15]))
    sheet.save(dest, quality=85)
    print("sheet:", dest)


def main():
    ap = argparse.ArgumentParser(description="run and compare gowa test scenarios")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("run")
    p.add_argument("scenario", choices=sorted(SCENARIOS))
    p.add_argument("--name")
    p.add_argument("--note")
    p.add_argument("--secs", type=int)
    p.add_argument("--snap-every", type=float)
    p.add_argument("--snap-from", type=float)
    p.add_argument("--trace")
    p.add_argument("--env", action="append")
    p.add_argument("--keep-save", action="store_true")
    p.add_argument("--save", help="savedata dir to restore instead of run/savedata_golden (e.g. out/play_save)")
    p = sub.add_parser("show")
    p.add_argument("run", nargs="?", default="last")
    sub.add_parser("list")
    p = sub.add_parser("compare")
    p.add_argument("a")
    p.add_argument("b", nargs="?", default="last")
    p.add_argument("--max", type=int, default=8)
    args = ap.parse_args()
    {"run": cmd_run, "show": cmd_show, "list": cmd_list, "compare": cmd_compare}[args.cmd](args)


if __name__ == "__main__":
    main()
