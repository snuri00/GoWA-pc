import argparse
import datetime
import os
import re
import shutil
import subprocess
import sys
import time

from threadmon import ThreadMon

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN_DIR = os.path.join(ROOT, "run")
EXE = os.path.join(ROOT, "build", "gowa.exe")
SAVE_DIR = os.path.join(RUN_DIR, "vfs", "dev_hdd0", "home", "00000001", "savedata")
PLAY_SAVE = os.path.join(ROOT, "out", "play_save")
SESSIONS = os.path.join(ROOT, "out", "play")


def copy_saves(src, dst):
    if not os.path.isdir(src):
        return
    os.makedirs(dst, exist_ok=True)
    for d in os.listdir(src):
        shutil.rmtree(os.path.join(dst, d), ignore_errors=True)
        shutil.copytree(os.path.join(src, d), os.path.join(dst, d))


def running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq gowa.exe"], capture_output=True, text=True).stdout
    return "gowa.exe" in out


def last_flip(log_path, state):
    try:
        with open(log_path, "rb") as f:
            f.seek(state.get("pos", 0))
            data = f.read()
            state["pos"] = f.tell()
    except OSError:
        return state.get("flip")
    for m in re.finditer(rb"I +([0-9.]+) [^\r\n]*rsx: flip (\d+)[^\r\n]*worst ([0-9.]+)", data):
        t, n = float(m.group(1)), int(m.group(2))
        if "flip" in state and n > state["n"]:
            state["fps"] = (n - state["n"]) / max(t - state["flip"], 1e-3)
        state["worst"] = float(m.group(3))
        state["flip"], state["n"] = t, n
    return state.get("flip")


def summarize(rows, path):
    def avg(sel):
        acc = {}
        for r in sel:
            for k, v in r[2].items():
                acc[k] = acc.get(k, 0) + v
        return {k: v / len(sel) for k, v in acc.items()} if sel else {}
    played = [r for r in rows if r[0] > 90 and r[1] > 0]
    low = [r for r in played if r[1] < 24]
    high = [r for r in played if r[1] >= 28]
    with open(path, "w") as f:
        f.write("seconds %d, low(<24fps) %d, high(>=28fps) %d\n" % (len(played), len(low), len(high)))
        lo, hi = avg(low), avg(high)
        for k in sorted(set(lo) | set(hi), key=lambda k: -lo.get(k, 0)):
            if lo.get(k, 0) > 0.05 or hi.get(k, 0) > 0.05:
                f.write("%-28s low %5.1f%%  high %5.1f%%\n" % (k, 100 * lo.get(k, 0), 100 * hi.get(k, 0)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scale", type=float, default=0)
    ap.add_argument("--fps-unlock", action="store_true")
    ap.add_argument("--fresh-save", action="store_true")
    ap.add_argument("--env", action="append")
    ap.add_argument("--lowfps-capture", default="22:6:90:4", help="fps:secs:cooldown:max, 0 disables")
    args = ap.parse_args()
    if running():
        print("gowa.exe is already running; close it first")
        return 1
    if os.path.isdir(PLAY_SAVE) and not args.fresh_save:
        copy_saves(PLAY_SAVE, SAVE_DIR)
        print("restored your save from", PLAY_SAVE)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = os.path.join(SESSIONS, stamp)
    os.makedirs(out, exist_ok=True)
    env = dict(os.environ)
    env["GOWA_ROOT"] = ROOT
    for k in ("GOWA_AUTOPAD", "GOWA_SNAP_EVERY", "GOWA_SNAP_FROM", "GOWA_SNAP_DIR"):
        env.pop(k, None)
    if args.lowfps_capture != "0":
        env["GOWA_SPU_CAPTURE_LOWFPS"] = args.lowfps_capture
        env["GOWA_SPU_CAPTURE_DIR"] = os.path.join(out, "spucap")
    if args.scale:
        env["GOWA_RES_SCALE"] = str(args.scale)
    if args.fps_unlock:
        env["GOWA_FPS_UNLOCK"] = "1"
    for kv in args.env or []:
        k, _, v = kv.partition("=")
        env[k] = v
    for f in ("blackbox.req", "blackbox.done"):
        if os.path.exists(os.path.join(RUN_DIR, f)):
            os.remove(os.path.join(RUN_DIR, f))
    shutil.rmtree(os.path.join(RUN_DIR, "blackbox"), ignore_errors=True)
    print("starting the game; close its window to end the session")
    t0 = time.time()
    proc = subprocess.Popen([EXE], cwd=RUN_DIR, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    log_path = os.path.join(RUN_DIR, "gowa.log")
    state = {}
    requested = False
    mon = ThreadMon(proc.pid)
    rows = []
    mlog = open(os.path.join(out, "threads.log"), "w")
    last_t = time.time()
    while proc.poll() is None:
        time.sleep(1)
        now = time.time() - t0
        flip = last_flip(log_path, state)
        try:
            cpu = mon.sample()
        except OSError:
            cpu = {}
        dt = time.time() - last_t
        last_t = time.time()
        fps = state.get("fps", 0)
        load = {k: v / dt for k, v in cpu.items()}
        rows.append((now, fps, load))
        top = sorted(load.items(), key=lambda kv: -kv[1])[:10]
        mlog.write("%7.1f fps %5.1f worst %5.0f  %s\n" % (now, fps, state.get("worst", 0), "  ".join("%s=%.0f" % (k, 100 * v) for k, v in top)))
        mlog.flush()
        if not requested and flip is not None and now > 40 and now - flip > 45:
            requested = True
            open(os.path.join(RUN_DIR, "blackbox.req"), "w").close()
            print("no frames for %.0f s, capturing a blackbox" % (now - flip))
    code = proc.returncode
    mlog.close()
    summarize(rows, os.path.join(out, "threads_summary.txt"))
    copy_saves(SAVE_DIR, PLAY_SAVE)
    if os.path.exists(log_path):
        shutil.copy(log_path, os.path.join(out, "gowa.log"))
    box = os.path.join(RUN_DIR, "blackbox")
    if os.path.isdir(box):
        shutil.copytree(box, os.path.join(out, "blackbox"), dirs_exist_ok=True)
    print("session ended (exit code %d, %.0f s); log and captures in %s" % (code, time.time() - t0, out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
