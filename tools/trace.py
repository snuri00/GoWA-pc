import argparse
import json
import os
import sys

import numpy as np

T_THREAD, T_SYSCALL, T_IMPORT, T_FS_OPEN, T_FS_READ, T_FS_WRITE, T_COPY, T_SEMA, T_REPORT, T_FLIP, T_TEX, T_DMA, T_TRAP, T_DRAW, T_MARK, T_ATOMIC, T_FS_CLOSE = range(1, 18)
TYPE_NAMES = {T_THREAD: "thread", T_SYSCALL: "sys", T_IMPORT: "import", T_FS_OPEN: "open", T_FS_READ: "read", T_FS_WRITE: "write", T_COPY: "copy", T_SEMA: "sema", T_REPORT: "report",
              T_FLIP: "flip", T_TEX: "tex", T_DMA: "dma", T_TRAP: "trap", T_DRAW: "draw", T_MARK: "mark", T_ATOMIC: "atomic", T_FS_CLOSE: "close"}
TEX_KINDS = ["dummy", "memory", "depth redirect", "feedback copy", "surface region", "depth surface", "color surface"]
COPY_NAMES = {1: "nv3089", 2: "nv0039", 3: "nv308a"}

DT = np.dtype([("t", "<u8"), ("type", "<u2"), ("tid", "<u2"), ("x", "<u4"), ("a", "<u8"), ("b", "<u8"), ("c", "<u8"), ("d", "<u8"), ("e", "<u8")])
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Trace:
    def __init__(self, path):
        with open(path, "rb") as f:
            hdr = f.read(16)
        if hdr[:8] != b"GOWATRC1":
            sys.exit("not a trace file: " + path)
        size = DT.itemsize
        n = (os.path.getsize(path) - 16) // size
        self.r = np.memmap(path, dtype=DT, mode="r", offset=16, shape=(n,))
        order = np.argsort(self.r["t"], kind="stable")
        self.r = np.asarray(self.r)[order]
        self.strings = {}
        if os.path.exists(path + ".str"):
            with open(path + ".str", encoding="utf-8", errors="replace") as f:
                for line in f:
                    k, _, v = line.rstrip("\n").partition("\t")
                    if k.isdigit():
                        self.strings[int(k)] = v.replace("\\n", "\n").replace("\\t", "\t").replace("\\\\", "\\")
        self.threads = {}
        for rec in self.r[self.r["type"] == T_THREAD]:
            self.threads[int(rec["tid"])] = self.strings.get(int(rec["a"]), "?")
        sc = os.path.join(ROOT, "ref", "syscalls.json")
        self.sysnames = {int(k): v for k, v in json.load(open(sc)).items()} if os.path.exists(sc) else {}

    def s(self, i):
        return self.strings.get(int(i), "#%d" % int(i))

    def thread(self, tid):
        return self.threads.get(int(tid), "t%d" % int(tid))

    def select(self, args, types=None):
        r = self.r
        m = np.ones(len(r), bool)
        if types is not None:
            m &= np.isin(r["type"], list(types))
        if getattr(args, "start", None) is not None:
            m &= r["t"] >= int(args.start * 1e9)
        if getattr(args, "end", None) is not None:
            m &= r["t"] < int(args.end * 1e9)
        if getattr(args, "thread", None):
            tids = [k for k, v in self.threads.items() if args.thread.lower() in v.lower()]
            m &= np.isin(r["tid"], tids)
        return r[m]

    def fmt(self, e):
        t = int(e["type"])
        x, a, b, c, d, ee = (int(e[k]) for k in ("x", "a", "b", "c", "d", "e"))
        if t == T_SYSCALL:
            return "%s(0x%x, 0x%x, 0x%x) = 0x%x lr 0x%08x %dus" % (self.sysnames.get(x, "sys_%d" % x), a, b, c, d & 0xFFFFFFFFFFFFFFFF, ee >> 32, ee & 0xFFFFFFFF)
        if t == T_IMPORT:
            return "%s(0x%x, 0x%x, 0x%x) = 0x%x lr 0x%08x %dus" % (self.s(x), a, b, c, d, ee >> 32, ee & 0xFFFFFFFF)
        if t == T_FS_OPEN:
            return "open '%s' flags 0%o -> fd %d" % (self.s(x), a, b)
        if t == T_FS_CLOSE:
            return "close '%s' fd %d" % (self.s(x), a)
        if t == T_FS_READ:
            return "read '%s' pos 0x%x n 0x%x -> 0x%08x got 0x%x" % (self.s(x), a, b, c, d)
        if t == T_FS_WRITE:
            return "write '%s' pos 0x%x n 0x%x <- 0x%08x" % (self.s(x), a, b, c)
        if t == T_COPY:
            eng = x & 0xFF
            if eng == 1:
                x0, y0, x1, y1 = c & 0xFFFF, (c >> 16) & 0xFFFF, (c >> 32) & 0xFFFF, c >> 48
                return "nv3089 0x%08x -> 0x%08x rect %d,%d-%d,%d pitch 0x%x/0x%x bpp %d/%d%s%s" % (a, b, x0, y0, x1, y1, d & 0xFFFFFFFF, d >> 32, (ee >> 32) & 0xFF, (ee >> 40) & 0xFF,
                                                                                         " swizzled" if (ee >> 48) & 1 else "", " scaled" if (ee >> 49) & 1 else "")
            if eng == 2:
                return "nv0039 0x%08x -> 0x%08x len 0x%x lines %d pitch %d/%d" % (a, b, c, d, np.int32(ee & 0xFFFFFFFF), np.int32(ee >> 32))
            return "nv308a 0x%08x = 0x%08x" % (a, ee)
        if t == T_SEMA:
            return "sema %s 0x%08x %s 0x%x%s" % ({0x68: "acquire", 0x6C: "release", 0x1D70: "backend release", 0x1D74: "texture release"}.get(x, hex(x)), a, "==" if x == 0x68 else "=", b,
                                               " (now 0x%x)" % c if x == 0x68 else "")
        if t == T_REPORT:
            return "report type %d 0x%08x value 0x%x" % (x, a, b)
        if t == T_FLIP:
            return "flip %d buffer %d" % (a, x)
        if t == T_TEX:
            return "tex%d 0x%08x fmt 0x%08x %dx%d c0 0x%08x %s" % (x & 0xFF, a, b, c >> 16, c & 0xFFFF, d, TEX_KINDS[(x >> 8) & 7] if ((x >> 8) & 7) < 7 else "?")
        if t == T_DMA:
            return "spu%d %s cmd 0x%02x ea 0x%08x lsa 0x%05x size 0x%x pc 0x%05x" % (x & 0xFF, "PUT" if (x >> 8) & 1 else "GET", (x >> 16) & 0xFF, a, b, c, d)
        if t == T_ATOMIC:
            return "spu%d atomic 0x%02x line 0x%08x lsa 0x%05x pc 0x%05x" % (x & 0xFF, (x >> 16) & 0xFF, a, b, d)
        if t == T_TRAP:
            if x & 0x10000:
                return "trap %d %s burst 0x%08x-0x%08x %d accesses" % (x & 0xFF, "write" if (x >> 8) & 1 else "read", a, b, d)
            st = self.s(c) if c else ""
            return "trap %d %s 0x%08x = 0x%08x%s" % (x & 0xFF, "write" if (x >> 8) & 1 else "read", a, b, ("\n" + " " * 30 + st) if st else "")
        if t == T_MARK:
            return "mark %s" % self.s(a)
        return "%s x 0x%x a 0x%x b 0x%x c 0x%x d 0x%x e 0x%x" % (TYPE_NAMES.get(t, t), x, a, b, c, d, ee)

    def line(self, e):
        return "%11.6f %-26s %s" % (int(e["t"]) / 1e9, self.thread(e["tid"])[:26], self.fmt(e))


def tex_size(fmt, rect):
    f = (fmt >> 8) & 0x9F
    w, h = rect >> 16, rect & 0xFFFF
    mips = max(1, fmt >> 16)
    bpb = {0x86: 8, 0x87: 16, 0x88: 16}.get(f)
    total = 0
    for _ in range(mips):
        total += ((w + 3) // 4) * ((h + 3) // 4) * bpb if bpb else w * h * 4
        w, h = max(1, w // 2), max(1, h // 2)
    return total


def write_spans(r):
    t = r["type"]
    lo = np.zeros(len(r), np.uint64)
    hi = np.zeros(len(r), np.uint64)
    m = t == T_FS_READ
    lo[m], hi[m] = r["c"][m], r["c"][m] + r["d"][m]
    m = (t == T_DMA) & (((r["x"] >> 8) & 1) == 1)
    lo[m], hi[m] = r["a"][m], r["a"][m] + r["c"][m]
    m = t == T_ATOMIC
    lo[m], hi[m] = r["a"][m], r["a"][m] + 128
    m = (t == T_SEMA) & (r["x"] != 0x68)
    lo[m], hi[m] = r["a"][m], r["a"][m] + 4
    m = t == T_REPORT
    lo[m], hi[m] = r["a"][m], r["a"][m] + 16
    m = (t == T_TRAP) & ((r["x"] & 0x10000) == 0) & (((r["x"] >> 8) & 1) == 1)
    lo[m], hi[m] = r["a"][m], r["a"][m] + 4
    m = (t == T_TRAP) & ((r["x"] & 0x10000) != 0) & (((r["x"] >> 8) & 1) == 1)
    lo[m], hi[m] = r["a"][m], r["b"][m] + 16
    for i in np.nonzero(t == T_COPY)[0]:
        e = r[i]
        eng = int(e["x"]) & 0xFF
        a, b, c, d, ee = (int(e[k]) for k in ("a", "b", "c", "d", "e"))
        if eng == 1:
            x0, y0, x1, y1 = c & 0xFFFF, (c >> 16) & 0xFFFF, (c >> 32) & 0xFFFF, c >> 48
            bpp, dp = (ee >> 40) & 0xFF, d >> 32
            if (ee >> 48) & 1:
                lo[i], hi[i] = b, b + x1 * y1 * bpp * 4
            else:
                lo[i], hi[i] = b + y0 * dp + x0 * bpp, b + (y1 - 1) * dp + x1 * bpp
        elif eng == 2:
            op = int(np.int32(ee >> 32))
            ost = (int(e["x"]) >> 16) & 0xFF
            span = (d - 1) * op
            lo[i], hi[i] = (b + min(0, span)), b + max(0, span) + c * ost
        else:
            lo[i], hi[i] = a, a + max(1, c)
    return lo, hi


def read_spans(r):
    t = r["type"]
    lo = np.zeros(len(r), np.uint64)
    hi = np.zeros(len(r), np.uint64)
    m = t == T_FS_WRITE
    lo[m], hi[m] = r["c"][m], r["c"][m] + r["b"][m]
    m = (t == T_DMA) & (((r["x"] >> 8) & 1) == 0)
    lo[m], hi[m] = r["a"][m], r["a"][m] + r["c"][m]
    m = (t == T_TRAP) & ((r["x"] & 0x10000) == 0) & (((r["x"] >> 8) & 1) == 0)
    lo[m], hi[m] = r["a"][m], r["a"][m] + 4
    for i in np.nonzero(t == T_TEX)[0]:
        e = r[i]
        lo[i], hi[i] = int(e["a"]), int(e["a"]) + tex_size(int(e["b"]), int(e["c"]))
    for i in np.nonzero(t == T_COPY)[0]:
        e = r[i]
        eng = int(e["x"]) & 0xFF
        a, c, d, ee = (int(e[k]) for k in ("a", "c", "d", "e"))
        if eng == 1:
            x0, y0, x1, y1 = c & 0xFFFF, (c >> 16) & 0xFFFF, (c >> 32) & 0xFFFF, c >> 48
            ox, oy, sbpp, ip = ee & 0xFFFF, (ee >> 16) & 0xFFFF, (ee >> 32) & 0xFF, d & 0xFFFFFFFF
            if (ee >> 49) & 1:
                lo[i], hi[i] = a, a + ip * (y1 + 1)
            else:
                lo[i], hi[i] = a + (y0 - oy) * ip + (x0 - ox) * sbpp, a + (y1 - 1 - oy) * ip + (x1 - ox) * sbpp
        elif eng == 2:
            ip = int(np.int32(ee & 0xFFFFFFFF))
            ist = (int(e["x"]) >> 8) & 0xFF
            span = (d - 1) * ip
            lo[i], hi[i] = a + min(0, span), a + max(0, span) + c * ist
    return lo, hi


def copy_source(e, addr):
    eng = int(e["x"]) & 0xFF
    a, b, c, d, ee = (int(e[k]) for k in ("a", "b", "c", "d", "e"))
    if eng == 1:
        if (ee >> 48) & 1 or (ee >> 49) & 1:
            return None, "swizzled/scaled copy, cannot map exactly"
        x0, y0, x1, y1 = c & 0xFFFF, (c >> 16) & 0xFFFF, (c >> 32) & 0xFFFF, c >> 48
        ox, oy, sbpp, dbpp, ip, dp = ee & 0xFFFF, (ee >> 16) & 0xFFFF, (ee >> 32) & 0xFF, (ee >> 40) & 0xFF, d & 0xFFFFFFFF, d >> 32
        off = addr - b
        if off < 0 or dp == 0:
            return None, "outside"
        y, xb = divmod(off, dp)
        x, sub = divmod(xb, dbpp)
        if not (x0 <= x < x1 and y0 <= y < y1):
            return None, "outside"
        if sbpp != dbpp:
            return a + (y - oy) * ip + (x - ox) * sbpp, "format conversion"
        return a + (y - oy) * ip + (x - ox) * sbpp + sub, ""
    if eng == 2:
        ip, op = int(np.int32(ee & 0xFFFFFFFF)), int(np.int32(ee >> 32))
        ist, ost = (int(e["x"]) >> 8) & 0xFF, (int(e["x"]) >> 16) & 0xFF
        for line in range(d):
            base = b + line * op
            col = addr - base
            if 0 <= col < c * ost and col % ost == 0:
                return a + line * ip + (col // ost) * ist, ""
        return None, "outside"
    return None, "fill"


def exact_hit(e, addr):
    if int(e["type"]) == T_COPY:
        src, why = copy_source(e, addr)
        return src is not None or why not in ("outside",)
    return True


def parse_addr(s):
    return int(s, 16)


def cmd_info(tr, args):
    r = tr.r
    print("records %d, %.3fs - %.3fs" % (len(r), r["t"][0] / 1e9 if len(r) else 0, r["t"][-1] / 1e9 if len(r) else 0))
    types, counts = np.unique(r["type"], return_counts=True)
    for t, c in zip(types, counts):
        print("  %-8s %d" % (TYPE_NAMES.get(int(t), t), c))
    print("threads:")
    tids, counts = np.unique(r["tid"], return_counts=True)
    for t, c in sorted(zip(tids, counts), key=lambda z: -z[1]):
        print("  %-40s %d" % (tr.thread(t), c))


def cmd_timeline(tr, args):
    types = None
    if args.types:
        inv = {v: k for k, v in TYPE_NAMES.items()}
        types = [inv[t] for t in args.types.split(",")]
    r = tr.select(args, types)
    r = r[r["type"] != T_THREAD]
    if args.grep:
        out = [tr.line(e) for e in r]
        out = [l for l in out if args.grep in l]
    else:
        out = (tr.line(e) for e in r[: args.limit])
    for i, l in enumerate(out):
        if i >= args.limit:
            break
        print(l)


def cmd_top(tr, args):
    t = {"sys": T_SYSCALL, "import": T_IMPORT}[args.kind]
    r = tr.select(args, [t])
    keys, counts = np.unique(r["x"], return_counts=True)
    dur = {k: int((r["e"][r["x"] == k] & 0xFFFFFFFF).sum()) for k in keys}
    span = max(1e-9, (r["t"][-1] - r["t"][0]) / 1e9) if len(r) else 1
    print("%-48s %10s %10s %12s" % ("name", "count", "per sec", "total ms"))
    for k, c in sorted(zip(keys, counts), key=lambda z: -z[1])[: args.limit]:
        name = tr.sysnames.get(int(k), "sys_%d" % k) if t == T_SYSCALL else tr.s(k)
        print("%-48s %10d %10.1f %12.1f" % (name[:48], c, c / span, dur[k] / 1000))


def cmd_access(tr, args, writes):
    addr = parse_addr(args.addr)
    ln = parse_addr(args.len) if args.len else 1
    r = tr.select(args)
    lo, hi = write_spans(r) if writes else read_spans(r)
    m = (lo < addr + ln) & (hi > addr) & (hi > lo)
    n = 0
    for e in r[m]:
        if ln == 1 and writes and not exact_hit(e, addr):
            continue
        print(tr.line(e))
        n += 1
        if n >= args.limit:
            break
    if not n:
        print("no recorded %s; PPU stores are only visible through traps (GOWA_TRAP)" % ("writers" if writes else "readers"))


def cmd_chain(tr, args):
    addr = parse_addr(args.addr)
    t_lim = int(args.at * 1e9) if args.at is not None else 1 << 63
    r = tr.r
    r = r[~((r["type"] == T_TRAP) & ((r["x"] & 0x10000) != 0))]
    lo, hi = write_spans(r)
    for depth in range(args.depth):
        m = (lo <= addr) & (hi > addr) & (r["t"] < t_lim)
        idx = np.nonzero(m)[0][::-1]
        found = None
        for i in idx:
            if exact_hit(r[i], addr):
                found = i
                break
        if found is None:
            print("%s0x%08x: no recorded writer before %.6fs (PPU stores need a trap)" % ("  " * depth, addr, t_lim / 1e9 if t_lim < (1 << 63) else float("inf")))
            return
        e = r[found]
        print("%s0x%08x <- %s" % ("  " * depth, addr, tr.line(e)))
        t = int(e["type"])
        if t == T_COPY:
            src, why = copy_source(e, addr)
            if src is None:
                print("%s   (%s)" % ("  " * depth, why))
                return
            if why:
                print("%s   (%s)" % ("  " * depth, why))
            addr, t_lim = src, int(e["t"])
            continue
        if t == T_FS_READ:
            print("%s   file '%s' offset 0x%x" % ("  " * depth, tr.s(e["x"]), int(e["a"]) + addr - int(e["c"])))
        elif t == T_DMA:
            print("%s   from spu%d local store 0x%05x" % ("  " * depth, int(e["x"]) & 0xFF, int(e["b"]) + addr - int(e["a"])))
        return


def cmd_fs(tr, args):
    r = tr.select(args, [T_FS_READ, T_FS_WRITE, T_FS_OPEN])
    if args.path:
        ids = [k for k, v in tr.strings.items() if args.path.lower() in v.lower()]
        r = r[np.isin(r["x"], ids)]
    if args.summary:
        for t in (T_FS_READ, T_FS_WRITE):
            q = r[r["type"] == t]
            keys = np.unique(q["x"])
            for k in keys:
                qq = q[q["x"] == k]
                span = max(1e-9, (qq["t"][-1] - qq["t"][0]) / 1e9)
                tot = int(qq["b"].sum())
                print("%-6s %-60s %7d ops %10.1f KB %8.1f KB/s" % (TYPE_NAMES[t], tr.s(k)[-60:], len(qq), tot / 1024, tot / 1024 / span))
        return
    for e in r[: args.limit]:
        print(tr.line(e))


def cmd_gaps(tr, args):
    r = tr.select(args)
    r = r[r["type"] != T_THREAD]
    if args.grep:
        r = np.array([e for e in r if args.grep in tr.fmt(e)], dtype=DT)
    if len(r) < 2:
        print("not enough events")
        return
    t = r["t"].astype(np.int64)
    d = np.diff(t)
    print("events %d, median gap %.3f ms, max %.3f ms" % (len(r), np.median(d) / 1e6, d.max() / 1e6))
    for i in np.argsort(d)[::-1][: args.limit]:
        print("gap %9.3f ms after %s" % (d[i] / 1e6, tr.line(r[i])))


def main():
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--file", default=argparse.SUPPRESS)
    common.add_argument("--start", type=float, default=argparse.SUPPRESS)
    common.add_argument("--end", type=float, default=argparse.SUPPRESS)
    common.add_argument("--thread", default=argparse.SUPPRESS)
    common.add_argument("--limit", type=int, default=argparse.SUPPRESS)
    ap = argparse.ArgumentParser(description="query a gowa event trace", parents=[common])
    sub = ap.add_subparsers(dest="cmd", required=True)
    real_add = sub.add_parser
    sub.add_parser = lambda name, **kw: real_add(name, parents=[common], **kw)
    sub.add_parser("info")
    p = sub.add_parser("timeline")
    p.add_argument("--types")
    p.add_argument("--grep")
    p = sub.add_parser("top")
    p.add_argument("kind", choices=["sys", "import"])
    for name in ("writers", "readers"):
        p = sub.add_parser(name)
        p.add_argument("addr")
        p.add_argument("len", nargs="?")
    p = sub.add_parser("chain")
    p.add_argument("addr")
    p.add_argument("--at", type=float)
    p.add_argument("--depth", type=int, default=12)
    p = sub.add_parser("fs")
    p.add_argument("--path")
    p.add_argument("--summary", action="store_true")
    p = sub.add_parser("gaps")
    p.add_argument("--grep")
    args = ap.parse_args()
    for k, v in {"file": "run/trace.bin", "start": None, "end": None, "thread": None, "limit": 60}.items():
        if not hasattr(args, k):
            setattr(args, k, v)
    path = args.file if os.path.isabs(args.file) else os.path.join(ROOT, args.file) if not os.path.exists(args.file) else args.file
    tr = Trace(path)
    if args.cmd == "info":
        cmd_info(tr, args)
    elif args.cmd == "timeline":
        cmd_timeline(tr, args)
    elif args.cmd == "top":
        cmd_top(tr, args)
    elif args.cmd == "writers":
        cmd_access(tr, args, True)
    elif args.cmd == "readers":
        cmd_access(tr, args, False)
    elif args.cmd == "chain":
        cmd_chain(tr, args)
    elif args.cmd == "fs":
        cmd_fs(tr, args)
    elif args.cmd == "gaps":
        cmd_gaps(tr, args)


if __name__ == "__main__":
    main()
