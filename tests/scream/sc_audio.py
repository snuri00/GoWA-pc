import sys

import numpy as np

RATE = 48000
BANDS = [0, 60, 250, 1000, 4000, 8000, 16000, 24000]


def load(path):
    x = np.fromfile(path, dtype="<f4")
    return x[: len(x) // 2 * 2].reshape(-1, 2)


def stats(path, skip, secs):
    x = load(path)
    nan = int(np.count_nonzero(~np.isfinite(x)))
    x = np.nan_to_num(x)
    x = x[int(skip * RATE):]
    if secs:
        x = x[: int(secs * RATE)]
    m = x.mean(axis=1)
    sec = RATE
    n = len(m) // sec
    rms = np.array([np.sqrt(np.mean(m[i * sec:(i + 1) * sec] ** 2)) for i in range(n)])
    silent = float(np.mean(rms < 1e-4)) if n else 1.0
    d = np.abs(np.diff(m))
    win = 480
    loc = np.sqrt(np.convolve(m ** 2, np.ones(win) / win, mode="same"))[1:]
    clicks = int(np.count_nonzero((d > 0.25) & (d > 8 * loc + 1e-3)))
    seg = 8192
    k = len(m) // seg
    spec = np.zeros(seg // 2 + 1)
    for i in range(k):
        spec += np.abs(np.fft.rfft(m[i * seg:(i + 1) * seg] * np.hanning(seg))) ** 2
    freqs = np.fft.rfftfreq(seg, 1.0 / RATE)
    tot = spec.sum() + 1e-30
    bands = [float(spec[(freqs >= a) & (freqs < b)].sum() / tot) for a, b in zip(BANDS[:-1], BANDS[1:])]
    return dict(seconds=len(m) / RATE, nan=nan, rms=float(np.sqrt(np.mean(m ** 2))), peak=float(np.max(np.abs(x))) if len(x) else 0.0, clip=float(np.mean(np.abs(x) >= 0.999)), silent_secs=silent, clicks=clicks, rms_sec=rms, bands=bands)


def envelope(path, skip, secs):
    m = np.nan_to_num(load(path)).mean(axis=1)[int(skip * RATE):]
    if secs:
        m = m[: int(secs * RATE)]
    w = RATE // 10
    n = len(m) // w
    return np.sqrt((m[: n * w].reshape(n, w) ** 2).mean(axis=1))


def best_lag(a, b, maxlag):
    best = (0, -1.0)
    for lag in range(-maxlag, maxlag + 1):
        x = a[max(0, lag):]
        y = b[max(0, -lag):]
        n = min(len(x), len(y))
        if n < 50:
            continue
        c = np.corrcoef(x[:n], y[:n])[0, 1]
        if c > best[1]:
            best = (lag, c)
    return best


def main():
    skip = float(sys.argv[3]) if len(sys.argv) > 3 else 70.0
    secs = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0
    res = [stats(p, skip, secs) for p in sys.argv[1:3]]
    for p, r in zip(sys.argv[1:3], res):
        print("%s: %.1f s nan %d rms %.4f peak %.3f clipped %.5f silent-seconds %.2f clicks %d" % (p, r["seconds"], r["nan"], r["rms"], r["peak"], r["clip"], r["silent_secs"], r["clicks"]))
        print("   band energy %s" % " ".join("%d-%d:%.3f" % (a, b, e) for a, b, e in zip(BANDS[:-1], BANDS[1:], r["bands"])))
        q = np.percentile(r["rms_sec"], [10, 50, 90]) if len(r["rms_sec"]) else [0, 0, 0]
        print("   per-second rms p10 %.4f p50 %.4f p90 %.4f" % tuple(q))
    a, b = res
    ea, eb = envelope(sys.argv[1], skip, secs), envelope(sys.argv[2], skip, secs)
    lag, cor = best_lag(ea, eb, 100)
    print("envelope (100 ms rms) correlation %.3f at lag %.1f s" % (cor, lag / 10.0))
    rel = abs(a["rms"] - b["rms"]) / max(a["rms"], 1e-9)
    bd = max(abs(x - y) for x, y in zip(a["bands"], b["bands"]))
    print("rms relative difference %.3f, max band share difference %.3f" % (rel, bd))


if __name__ == "__main__":
    main()
