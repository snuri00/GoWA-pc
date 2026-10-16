def _d(op, rt, ra, imm):
    return (op << 26) | (rt << 21) | (ra << 16) | (imm & 0xFFFF)


def _x(op, rt, ra, rb, xo, rc=0):
    return (op << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc


def addi(rt, ra, imm):
    return _d(14, rt, ra, imm)


def li(rt, imm):
    return addi(rt, 0, imm)


def addis(rt, ra, imm):
    return _d(15, rt, ra, imm)


def lis(rt, imm):
    return addis(rt, 0, imm)


def ori(ra, rs, imm):
    return _d(24, rs, ra, imm)


def oris(ra, rs, imm):
    return _d(25, rs, ra, imm)


def addic_(rt, ra, imm):
    return _d(13, rt, ra, imm)


def lwz(rt, d, ra):
    return _d(32, rt, ra, d)


def stw(rs, d, ra):
    return _d(36, rs, ra, d)


def lfd(frt, d, ra):
    return _d(50, frt, ra, d)


def stfd(frs, d, ra):
    return _d(54, frs, ra, d)


def ld(rt, d, ra):
    assert d % 4 == 0
    return (58 << 26) | (rt << 21) | (ra << 16) | (d & 0xFFFC)


def std(rs, d, ra):
    assert d % 4 == 0
    return (62 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFC)


def lvx(vd, ra, rb):
    return _x(31, vd, ra, rb, 103)


def stvx(vs, ra, rb):
    return _x(31, vs, ra, rb, 231)


def mtcrf(fxm, rs):
    return (31 << 26) | (rs << 21) | (fxm << 12) | (144 << 1)


def mfcr(rt):
    return _x(31, rt, 0, 0, 19)


def _spr(n):
    return ((n & 31) << 16) | ((n >> 5) << 11)


def mtspr(spr, rs):
    return (31 << 26) | (rs << 21) | _spr(spr) | (467 << 1)


def mfspr(rt, spr):
    return (31 << 26) | (rt << 21) | _spr(spr) | (339 << 1)


def mtfsf(fm, frb):
    return (63 << 26) | (fm << 17) | (frb << 11) | (711 << 1)


def mffs(frt):
    return _x(63, frt, 0, 0, 583)


def mtvscr(vb):
    return (4 << 26) | (vb << 11) | 1604


def mfvscr(vd):
    return (4 << 26) | (vd << 21) | 1540


def bc(bo, bi, target_rel):
    return (16 << 26) | (bo << 21) | (bi << 16) | (target_rel & 0xFFFC)


def bne(target_rel, cr=0):
    return bc(4, cr * 4 + 2, target_rel)


def b(target_rel):
    return (18 << 26) | (target_rel & 0x03FFFFFC)


def sc():
    return 0x44000002


def nop():
    return 0x60000000


def load_imm32(rt, value):
    return [lis(rt, (value >> 16) & 0xFFFF), ori(rt, rt, value & 0xFFFF)]
