import argparse
import struct
import sys

import spu_dis


def f32(u):
    return struct.unpack(">f", struct.pack(">I", u & 0xFFFFFFFF))[0]


def fmt_const(words):
    if len(set(words)) == 1:
        w = words[0]
        f = f32(w)
        if w == 0:
            return "0"
        if 1e-6 < abs(f) < 1e7:
            return "%.7g[%08x]" % (f, w)
        return "0x%08x" % w
    return "{" + ",".join("%08x" % w for w in words) + "}"


class Lifter:
    def __init__(self, image, base):
        self.image = image
        self.base = base
        self.const = {}

    def word(self, pc):
        o = pc - self.base
        return struct.unpack_from(">I", self.image, o)[0]

    def reg(self, r):
        if r in self.const:
            return "r%d=%s" % (r, fmt_const(self.const[r]))
        return "r%d" % r

    def lift(self, pc):
        w = self.word(pc)
        text = spu_dis.dis(w, pc)
        parts = text.replace(",", " ").split()
        op = parts[0] if parts else "?"
        args = parts[1:]
        rt = None
        if args and args[0].startswith("r") and args[0][1:].isdigit():
            rt = int(args[0][1:])
        i16 = (w >> 7) & 0xFFFF
        if op == "il":
            v = i16 - 0x10000 if i16 & 0x8000 else i16
            self.const[rt] = [v & 0xFFFFFFFF] * 4
            return "r%d = %s" % (rt, fmt_const(self.const[rt]))
        if op == "ila":
            self.const[rt] = [(w >> 7) & 0x3FFFF] * 4
            return "r%d = %s" % (rt, fmt_const(self.const[rt]))
        if op == "ilhu":
            self.const[rt] = [i16 << 16] * 4
            return "r%d = %s" % (rt, fmt_const(self.const[rt]))
        if op == "ilh":
            self.const[rt] = [i16 | (i16 << 16)] * 4
            return "r%d = %s" % (rt, fmt_const(self.const[rt]))
        if op == "iohl" and rt in self.const:
            self.const[rt] = [x | i16 for x in self.const[rt]]
            return "r%d = %s" % (rt, fmt_const(self.const[rt]))
        if op == "fsmbi":
            m = i16
            ws = []
            for k in range(4):
                v = 0
                for b in range(4):
                    v = (v << 8) | (0xFF if (m >> (15 - (k * 4 + b))) & 1 else 0)
                ws.append(v)
            self.const[rt] = ws
            return "r%d = %s" % (rt, fmt_const(ws))
        operands = []
        for a in args[1:] if rt is not None else args:
            if a.startswith("r") and a[1:].isdigit():
                operands.append(self.reg(int(a[1:])))
            else:
                operands.append(a)
        if op in ("ori", "ai") and rt is not None and len(args) > 2 and args[2] in ("0", "0x0"):
            src = int(args[1][1:])
            if src in self.const:
                self.const[rt] = list(self.const[src])
            else:
                self.const.pop(rt, None)
            return "r%d = %s" % (rt, self.reg(src))
        if rt is not None and op not in ("stqd", "stqx", "stqa", "stqr", "wrch", "brz", "brnz", "brhz", "brhnz", "biz", "binz", "bihz", "bihnz", "hbrr", "hbra", "hbr", "nop", "lnop", "bi", "br", "bra", "brsl", "brasl"):
            self.const.pop(rt, None)
            return "r%d = %s(%s)" % (rt, op, ", ".join(operands))
        return "%s %s" % (op, ", ".join(operands if rt is None else [self.reg(rt)] + operands))


def main():
    ap = argparse.ArgumentParser(description="lift SPU code to readable pseudo code with constant tracking")
    ap.add_argument("elf")
    ap.add_argument("file_offset", type=lambda x: int(x, 16))
    ap.add_argument("ls_base", type=lambda x: int(x, 16))
    ap.add_argument("start", type=lambda x: int(x, 16))
    ap.add_argument("end", type=lambda x: int(x, 16))
    a = ap.parse_args()
    data = open(a.elf, "rb").read()
    lf = Lifter(data[a.file_offset:], a.ls_base)
    targets = set()
    for pc in range(a.ls_base, a.end, 4):
        t = spu_dis.dis(lf.word(pc), pc).replace(",", " ").split()
        if t and t[0] in ("br", "bra", "brz", "brnz", "brhz", "brhnz", "brsl", "brasl") and t[-1].startswith("0x"):
            targets.add(int(t[-1], 16))
    for pc in range(a.start, a.end, 4):
        if pc in targets:
            lf.const.clear()
            print("%05x: ---- label" % pc)
        line = lf.lift(pc)
        if line.startswith(("nop", "lnop", "hbr")):
            continue
        print("%05x: %s" % (pc, line))
    return 0


if __name__ == "__main__":
    sys.exit(main())
