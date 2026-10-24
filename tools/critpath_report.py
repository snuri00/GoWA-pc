import argparse
import bisect
import collections
import os
import struct
import sys

EV = struct.Struct("<QQIIIIIBBBBQ")

EV_FRAME, EV_WAIT, EV_SYSCALL, EV_JOB, EV_STORE, EV_FLIP, EV_KICK, EV_PUSH = 1, 2, 3, 5, 6, 7, 8, 9

KINDS = {
    2: "counter",
    3: "batch-finish",
    4: "batch-addjob",
    6: "wws-list",
    7: "edge-prev",
    8: "graph-spin",
    10: "rsx",
    12: "edge-guard",
}

IMAGES = [
    (0x6c3e00, 0x10000, "Task1(6c3e00)"),
    (0x6d1b00, 0xe000, "Task(6d1b00)"),
    (0x6db580, 0xa000, "ParticleTask"),
    (0x6e5900, 0xa000, "ZlibTask"),
    (0x6f4a00, 0x1800, "SortPrim"),
    (0x6f7200, 0x1600, "TexStreaming"),
    (0x6f8800, 0xce80, "PrimBuilder"),
    (0x705680, 0x16e80, "RenJob"),
    (0x71c500, 0xc300, "WaterProc"),
    (0x728400, 0x8400, "Cloth"),
    (0x730800, 0x4000, "TexCreator"),
    (0x734800, 0x2d80, "Img734800"),
    (0x737580, 0xc430, "Edge"),
    (0x743a00, 0x4a00, "ColorCorrection"),
    (0x748400, 0x2d00, "RsxWakeup"),
    (0x74b100, 0x9000, "MeshPrep"),
    (0x78b380, 0x8180, "MeshHierarchy"),
    (0x793500, 0x8f00, "GroupVis"),
    (0x79c400, 0x9400, "Shadows"),
    (0x7a5c80, 0x1000, "StereoDepth"),
    (0x7bfc80, 0x22000, "ScreamAux"),
    (0x7e1c00, 0x1e000, "ScreamMp3"),
    (0x84be40, 0x20000, "Anim"),
]
IMG_STARTS = [s for s, _, _ in IMAGES]


def site(d, s):
    if d.version < 2:
        return "sc%d@%08x" % (s[2], s[5])
    return "sc%d@%08x<%08x<%08x" % (s[2], s[5], s[3], s[4])


def image_name(src, wid):
    i = bisect.bisect_right(IMG_STARTS, src) - 1
    if i >= 0:
        s, n, name = IMAGES[i]
        if src < s + n:
            return name
    return "img%06x/w%d" % (src, wid)


class Data:
    def __init__(self, path):
        self.names = {}
        self.ev = collections.defaultdict(list)
        with open(path, "rb") as f:
            blob = f.read()
        if blob[:5] != b"CPATH" or blob[5:6] not in (b"1", b"2", b"3"):
            raise SystemExit("not a critpath file: %s" % path)
        self.version = int(blob[5:6])
        self.hz = struct.unpack_from("<d", blob, 8)[0]
        pos = 16
        n = len(blob)
        while pos + 8 <= n:
            tag, rid = struct.unpack_from("<II", blob, pos)
            if tag == 1:
                if pos + 48 > n:
                    break
                self.names[rid] = blob[pos + 8:pos + 48].split(b"\0")[0].decode("utf-8", "replace")
                pos += 48
            elif tag == 2:
                if pos + 12 > n:
                    break
                cnt = struct.unpack_from("<I", blob, pos + 8)[0]
                end = pos + 12 + cnt * EV.size
                if end > n:
                    break
                self.ev[rid].extend(EV.iter_unpack(blob[pos + 12:end]))
                pos = end
            else:
                break
        self.main = next((r for r, nm in self.names.items() if nm == "main_thread"), None)
        self.spu = {r for r, nm in self.names.items() if nm.startswith("SPU")}

    def ms(self, cycles):
        return cycles * 1000.0 / self.hz


def build(d, args):
    main = d.ev.get(d.main, [])
    frames = [e[0] for e in main if e[7] == EV_FRAME]
    if len(frames) < 3:
        raise SystemExit("too few frames (%d)" % len(frames))
    t_origin = min(e[0] for lst in d.ev.values() for e in lst if e[0])
    lo = t_origin + int(args.from_s * d.hz)
    hi = t_origin + int(args.to_s * d.hz) if args.to_s else 1 << 63
    frames = [t for t in frames if lo <= t <= hi]
    jobs = []
    stores = []
    for rid in d.spu:
        for e in d.ev[rid]:
            if e[7] == EV_JOB:
                jobs.append(e)
            elif e[7] == EV_STORE:
                stores.append(e)
    for rid, lst in d.ev.items():
        if rid in d.spu:
            continue
        for e in lst:
            if e[7] == EV_STORE:
                stores.append(e)
    jobs.sort(key=lambda e: e[1])
    stores.sort(key=lambda e: e[0])
    job_end = [e[1] for e in jobs]
    st_t = [e[0] for e in stores]
    by_slot = collections.defaultdict(list)
    for e in jobs:
        by_slot[e[4] & 0xFF].append(e)
    slot_end = {s: [e[1] for e in lst] for s, lst in by_slot.items()}
    waits = [e for e in main if e[7] == EV_WAIT and e[9] == 0 and e[1] > e[0]]
    sysc = outside(waits, [e for e in main if e[7] == EV_SYSCALL and e[9] == 0])
    return frames, jobs, job_end, stores, st_t, by_slot, slot_end, waits, sysc, t_origin


def outside(waits, sysc):
    merged = []
    for a, b in sorted((w[0], w[1]) for w in waits):
        if merged and a <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], b)
        else:
            merged.append([a, b])
    starts = [a for a, _ in merged]
    out = []
    for s in sysc:
        i = bisect.bisect_right(starts, s[0]) - 1
        if i >= 0 and merged[i][1] >= s[1]:
            continue
        out.append(s)
    return out


def job_split(d, j):
    wall = j[1] - j[0]
    if d.version < 2:
        return wall, 0, 0, 0, 0, 0
    lr = j[11] & 0xFFFFFFFF
    blk = j[11] >> 32
    ring = j[6]
    labn = j[3] >> 8 if d.version == 2 else (j[3] >> 8) & 0xFF
    idle = (j[4] >> 9) << 8
    return wall, lr, blk, ring, labn, idle


def job_emu(d, j):
    if d.version < 3:
        return 0, 0
    return (j[3] >> 16) << 4, j[10]


def job_for_store(st, by_slot, slot_end, hz):
    slot = (st[3] >> 16) & 0xFF
    lst = by_slot.get(slot)
    if not lst:
        return None
    ends = slot_end[slot]
    i = bisect.bisect_left(ends, st[0])
    if i < len(lst) and lst[i][0] <= st[0]:
        return lst[i]
    if i > 0 and st[0] - lst[i - 1][1] < hz * 0.002:
        return lst[i - 1]
    return None


def completer(w, ctx, d):
    frames, jobs, job_end, stores, st_t, by_slot, slot_end, waits, sysc, t_origin = ctx
    t0, t1, a, b, c = w[0], w[1], w[2], w[3], w[4]
    kind = w[8]
    if kind == 10:
        return None
    word = None
    if kind == 2:
        word = 0x9c7668
    elif kind == 6:
        word = (c + 64 + b * 16) if c else None
    i0 = bisect.bisect_left(st_t, t0)
    i1 = bisect.bisect_right(st_t, t1)
    best = None
    if kind in (2, 6):
        for k in range(i1 - 1, i0 - 1, -1):
            s = stores[k]
            if s[2] == word:
                best = s
                break
    elif kind in (3, 4):
        for k in range(i1 - 1, i0 - 1, -1):
            s = stores[k]
            if s[3] >> 24 == 11 and s[2] != 0x9c7668:
                best = s
                break
    if best is not None:
        j = job_for_store(best, by_slot, slot_end, d.hz)
        src = best[6]
        wid = best[3] >> 24
        return best[0], j, image_name(src, wid)
    k = bisect.bisect_right(job_end, t1) - 1
    if k >= 0 and job_end[k] >= t0:
        j = jobs[k]
        return j[1], j, image_name(j[2], j[3] & 0xFF)
    return None


def analyze(d, args):
    ctx = build(d, args)
    frames, jobs, job_end, stores, st_t, by_slot, slot_end, waits, sysc, t_origin = ctx
    nf = len(frames) - 1
    fr = []
    for i in range(nf):
        fr.append({
            "t0": frames[i],
            "t1": frames[i + 1],
            "wait": collections.Counter(),
            "wtype": collections.Counter(),
            "q": 0.0, "r": 0.0, "n": 0.0, "u": 0.0,
            "run": collections.Counter(),
            "busy": collections.Counter(),
            "lrw": collections.Counter(),
            "blkw": collections.Counter(),
            "ringw": collections.Counter(),
            "spin": collections.Counter(),
            "emu": collections.Counter(),
            "decl": collections.Counter(),
            "cnt": collections.Counter(),
            "sys": 0.0,
            "sysk": collections.Counter(),
            "dq": collections.Counter(),
            "dr": collections.Counter(),
            "dn": collections.Counter(),
            "dn_site": collections.Counter(),
            "longest": (0.0, ""),
        })
    starts = frames[:-1]

    def frame_of(t):
        i = bisect.bisect_right(starts, t) - 1
        return i if 0 <= i < nf and t < frames[i + 1] else None

    long_batch = []
    for w in waits:
        fi = frame_of(w[0])
        if fi is None:
            continue
        F = fr[fi]
        dur = d.ms(w[1] - w[0])
        kn = KINDS.get(w[8], "k%d" % w[8])
        F["wait"][kn] += dur
        if w[8] in (3, 4) and dur > args.batch_ms:
            long_batch.append(w)
        cp = completer(w, ctx, d)
        if cp is None:
            label = "rsx" if w[8] == 10 else "unknown"
            F["wtype"][label] += dur
            F["u"] += dur
            continue
        tc, j, name = cp
        tc = min(max(tc, w[0]), w[1])
        notify = d.ms(w[1] - tc)
        js = j[0] if j else w[0]
        queue = d.ms(max(0, min(js, tc) - w[0]))
        run = max(0.0, d.ms(tc - w[0]) - queue)
        F["q"] += queue
        F["r"] += run
        F["n"] += notify
        F["wtype"][name] += dur
        F["dq"][name] += queue
        F["dr"][name] += run
        F["dn"][name] += notify
        F["dn_site"][kn] += notify
        if dur > F["longest"][0]:
            F["longest"] = (dur, "%s<-%s" % (kn, name))
    for s in sysc:
        fi = frame_of(s[0])
        if fi is not None:
            fr[fi]["sys"] += d.ms(s[1] - s[0])
            fr[fi]["sysk"][site(d, s)] += d.ms(s[1] - s[0])
    for j in jobs:
        fi = frame_of(j[0])
        if fi is None:
            continue
        nm = image_name(j[2], j[3] & 0xFF)
        wall, lr, blk, ring, labn, _ = job_split(d, j)
        F = fr[fi]
        F["run"][nm] += d.ms(wall)
        F["busy"][nm] += d.ms(max(0, wall - lr - blk - ring))
        F["lrw"][nm] += d.ms(lr)
        F["blkw"][nm] += d.ms(blk)
        F["ringw"][nm] += d.ms(ring)
        F["spin"][nm] += 1 if labn > 1 else 0
        emu, decl = job_emu(d, j)
        F["emu"][nm] += emu
        F["decl"][nm] += decl
        F["cnt"][nm] += 1
    return fr, long_batch, ctx


def table(d, fr, title, out):
    if not fr:
        out.append("%s: no frames" % title)
        return
    n = len(fr)
    ft = sum(d.ms(F["t1"] - F["t0"]) for F in fr) / n
    wait = collections.Counter()
    wtype = collections.Counter()
    run = collections.Counter()
    cnt = collections.Counter()
    q = r = no = u = sy = 0.0
    for F in fr:
        wait.update(F["wait"])
        wtype.update(F["wtype"])
        run.update(F["run"])
        cnt.update(F["cnt"])
        q += F["q"]
        r += F["r"]
        no += F["n"]
        u += F["u"]
        sy += F["sys"]
    tw = sum(wait.values()) / n
    out.append("## %s: %d frames, avg frame %.2f ms (%.1f fps)" % (title, n, ft, 1000.0 / ft if ft else 0))
    out.append("PPU main thread waits: %.2f ms/frame = %.1f %% of frame time; main-thread blocking syscalls outside wait sites %.2f ms (%.1f %%)" % (tw, 100 * tw / ft, sy / n, 100 * sy / n / ft))
    out.append("  decomposition (ms/frame): queue %.2f, run %.2f, notify %.2f, unattributed %.2f" % (q / n, r / n, no / n, u / n))
    out.append("  | wait site | ms/frame | % frame |")
    out.append("  |---|---|---|")
    for k, v in wait.most_common():
        out.append("  | %s | %.2f | %.1f |" % (k, v / n, 100 * v / n / ft))
    dq, dr, dn, dns, sysk = collections.Counter(), collections.Counter(), collections.Counter(), collections.Counter(), collections.Counter()
    for F in fr:
        dq.update(F["dq"])
        dr.update(F["dr"])
        dn.update(F["dn"])
        dns.update(F["dn_site"])
        sysk.update(F["sysk"])
    out.append("  | last completer (job type) | wait ms/frame | % frame | queue | run | notify |")
    out.append("  |---|---|---|---|---|---|")
    for k, v in wtype.most_common(14):
        out.append("  | %s | %.2f | %.1f | %.2f | %.2f | %.2f |" % (k, v / n, 100 * v / n / ft, dq[k] / n, dr[k] / n, dn[k] / n))
    out.append("  notify by wait site (ms/frame): " + ", ".join("%s %.2f" % (k, v / n) for k, v in dns.most_common()))
    out.append("  main-thread blocking syscalls outside wait sites (ms/frame, site = lr<caller<caller): " + ", ".join("%s %.2f" % (k, v / n) for k, v in sysk.most_common(6)))
    busy, lrw, blkw, ringw, spin, emu, decl = (collections.Counter() for _ in range(7))
    for F in fr:
        emu.update(F["emu"])
        decl.update(F["decl"])
        busy.update(F["busy"])
        lrw.update(F["lrw"])
        blkw.update(F["blkw"])
        ringw.update(F["ringw"])
        spin.update(F["spin"])
    out.append("  SPU job time (ms/frame): wall = busy + event wait (rdch ch0: LR/SPURS events) + block wait (mailbox/signal/stop) + Edge ring spin (repeated RSX label 0x50300c80.. reads)")
    out.append("  emulated kinsn/job = SPU instructions run as recompiled/interpreted SPU code (not native HLE); HLE declines/job = native hooks that fell back to SPU code")
    out.append("  | job type | wall | busy | event wait | block wait | ring spin | jobs/frame | busy us/job | ring-spin jobs/frame | emulated kinsn/job | HLE declines/job |")
    out.append("  |---|---|---|---|---|---|---|---|---|---|---|")
    for k, v in run.most_common(16):
        c = cnt[k] or 1
        out.append("  | %s | %.2f | %.2f | %.2f | %.2f | %.2f | %.1f | %.0f | %.2f | %.1f | %.2f |" % (
            k, v / n, busy[k] / n, lrw[k] / n, blkw[k] / n, ringw[k] / n, cnt[k] / n, 1000 * busy[k] / c, spin[k] / n, emu[k] / c / 1000, decl[k] / c))
    tb, tl, tk, tr = (sum(x.values()) / n for x in (busy, lrw, blkw, ringw))
    out.append("  all jobs: wall %.1f ms/frame = %.2f SPUs (busy %.2f, event wait %.2f, block wait %.2f, ring spin %.2f SPUs)" % (
        sum(run.values()) / n, sum(run.values()) / n / ft, tb / ft, tl / ft, tk / ft, tr / ft))


def slot_table(d, ctx, out):
    frames, jobs, job_end, stores, st_t, by_slot, slot_end, waits, sysc, t_origin = ctx
    if len(frames) < 2:
        return
    lo, hi = frames[0], frames[-1]
    span = hi - lo
    out.append("## SPU threads (%.1f s window)" % (span / d.hz))
    out.append("| slot | in jobs % | job busy % | job event wait % | job block wait % | job ring spin % | idle wait between jobs % | kernel/PM (rest) % | jobs/s |")
    out.append("|---|---|---|---|---|---|---|---|---|")
    for slot in sorted(by_slot):
        acc = [0, 0, 0, 0, 0, 0]
        k = 0
        for j in by_slot[slot]:
            if j[0] < lo or j[0] >= hi:
                continue
            k += 1
            wall, lr, blk, ring, labn, idle = job_split(d, j)
            acc[0] += wall
            acc[1] += max(0, wall - lr - blk - ring)
            acc[2] += lr
            acc[3] += blk
            acc[4] += ring
            acc[5] += idle
        p = [100.0 * x / span for x in acc]
        out.append("| %d | %.1f | %.1f | %.1f | %.1f | %.1f | %.1f | %.1f | %.0f |" % (slot, p[0], p[1], p[2], p[3], p[4], p[5], max(0.0, 100 - p[0] - p[5]), k * d.hz / span))


def thread_table(d, ctx, out):
    frames = ctx[0]
    if len(frames) < 2:
        return
    lo, hi = frames[0], frames[-1]
    span = hi - lo
    out.append("## PPU threads: blocking syscalls >= 20 us (% of wall time; the main thread total includes syscalls inside wait sites)")
    out.append("| thread | blocked % | top sites (lr<caller<caller: %) |")
    out.append("|---|---|---|")
    rows = []
    for rid, lst in d.ev.items():
        if rid in d.spu:
            continue
        tot = 0
        sites = collections.Counter()
        for e in lst:
            if e[7] != EV_SYSCALL or e[1] <= lo or e[0] >= hi:
                continue
            dt = min(e[1], hi) - max(e[0], lo)
            tot += dt
            sites[site(d, e)] += dt
        if tot:
            rows.append((tot, d.names.get(rid, str(rid)), sites))
    for tot, name, sites in sorted(rows, key=lambda r: -r[0]):
        out.append("| %s | %.1f | %s |" % (name, 100.0 * tot / span, ", ".join("%s %.1f" % (k, 100.0 * v / span) for k, v in sites.most_common(3))))


def main():
    ap = argparse.ArgumentParser(description="Critical-path report from a GOWA_CRITPATH recording")
    ap.add_argument("path", help="critpath.bin or a run directory containing it")
    ap.add_argument("--from", dest="from_s", type=float, default=0.0, help="skip the first N seconds of the recording")
    ap.add_argument("--to", dest="to_s", type=float, default=0.0)
    ap.add_argument("--slow", type=float, default=25.0, help="fps threshold for slow frames")
    ap.add_argument("--frames", type=int, default=0, help="print per-frame lines (0 = none, N = first N, -1 = all)")
    ap.add_argument("--batch-ms", type=float, default=5.0, help="list batch waits longer than this")
    ap.add_argument("--out", help="write the report to this file too")
    args = ap.parse_args()
    path = args.path
    if os.path.isdir(path):
        path = os.path.join(path, "critpath.bin")
    d = Data(path)
    fr, long_batch, ctx = analyze(d, args)
    out = []
    out.append("# critical path: %s (tsc %.3f GHz, threads: %s)" % (path, d.hz / 1e9, ", ".join(sorted(set(d.names.values())))))
    slow = [F for F in fr if d.ms(F["t1"] - F["t0"]) > 1000.0 / args.slow]
    fast = [F for F in fr if d.ms(F["t1"] - F["t0"]) <= 1000.0 / args.slow]
    table(d, fr, "all frames", out)
    table(d, slow, "slow frames (< %.0f fps)" % args.slow, out)
    table(d, fast, "fast frames (>= %.0f fps)" % args.slow, out)
    slot_table(d, ctx, out)
    thread_table(d, ctx, out)
    if long_batch:
        out.append("## batch waits > %.1f ms" % args.batch_ms)
        t_origin = ctx[-1]
        for w in long_batch[:40]:
            mins = w[11]
            out.append("  t %.3f s %s %.2f ms obj %08x added %u->%u done %u lists %u mode %u list-mins %s sleeps %u" % (
                (w[0] - t_origin) / d.hz, KINDS.get(w[8]), d.ms(w[1] - w[0]), w[2], w[3], w[4], w[5], w[6] & 0xFFFF, (w[6] >> 16) & 0xFF,
                " ".join("%d" % ((mins >> (16 * i)) & 0xFFFF) for i in range(4)), w[10]))
    if args.frames:
        out.append("## per-frame")
        t_origin = ctx[-1]
        sel = fr if args.frames < 0 else fr[:args.frames]
        for F in sel:
            ft = d.ms(F["t1"] - F["t0"])
            w = sum(F["wait"].values())
            top = ", ".join("%s %.1f" % kv for kv in F["wait"].most_common(3))
            jt = ", ".join("%s %.1f" % kv for kv in F["wtype"].most_common(3))
            out.append("t %.3f frame %.1f ms wait %.1f (q %.1f r %.1f n %.1f u %.1f) [%s] completers [%s] longest %.1f %s" % (
                (F["t0"] - t_origin) / d.hz, ft, w, F["q"], F["r"], F["n"], F["u"], top, jt, F["longest"][0], F["longest"][1]))
    text = "\n".join(out)
    print(text)
    if args.out:
        with open(args.out, "w", newline="\n") as f:
            f.write(text + "\n")


if __name__ == "__main__":
    main()
