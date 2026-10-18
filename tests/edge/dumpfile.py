import struct

LS = 0x40000


class Snap:
    def __init__(self, regs, ls):
        self.regs = regs
        self.ls = ls

    def r(self, i, slot=0):
        o = i * 16 + (3 - slot) * 4
        return struct.unpack_from("<I", self.regs, o)[0]

    def q(self, i):
        return [self.r(i, k) for k in range(4)]

    def u8(self, a):
        return self.ls[a]

    def u16(self, a):
        return struct.unpack_from(">H", self.ls, a)[0]

    def u32(self, a):
        return struct.unpack_from(">I", self.ls, a)[0]

    def f32(self, a):
        return struct.unpack_from(">f", self.ls, a)[0]


class Hit:
    pass


def load(path):
    out = []
    with open(path, "rb") as f:
        while True:
            h = f.read(32)
            if len(h) < 32:
                break
            magic, fn, ret, sp, job, n, pure, _ = struct.unpack("<8I", h)
            hit = Hit()
            hit.fn, hit.ret, hit.sp, hit.job, hit.insns, hit.pure = fn, ret, sp, job, n, pure
            hit.before = Snap(f.read(2048), f.read(LS))
            hit.after = Snap(f.read(2048), f.read(LS))
            out.append(hit)
    return out


def diff_ranges(a, b, skip=None):
    rs = []
    i = 0
    n = len(a)
    while i < n:
        if a[i] != b[i] and not (skip and skip[0] <= i < skip[1]):
            j = i
            while j < n and (a[j] != b[j] or (j + 1 < n and a[j + 1] != b[j + 1])):
                j += 1
            rs.append((i, j))
            i = j
        i += 1
    return rs
