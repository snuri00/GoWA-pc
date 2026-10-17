import json
import os
import random
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "recomp", "ppu"))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
sys.path.insert(0, os.path.join(HERE, "..", "ppu_conformance"))

import asm
import spu_dis

LOAD_BASE = 0x10000
STATE = 0x800
BUF = 0x3F000
VAR_LSA, VAR_IN, VAR_OUT, VAR_CNT = 0x3FE00, 0x3FE10, 0x3FE20, 0x3FE30
CODE_LIMIT = 0x3E000
BASE_REG = 127

SKIP = {
    "stop", "lnop", "sync", "dsync", "mfspr", "rdch", "rchcnt", "mtspr", "wrch", "biz", "binz", "bihz", "bihnz", "stopd",
    "stqx", "bi", "bisl", "iret", "bisled", "hbr", "lqx", "heq", "hgt", "hlgt", "fscrrd", "fscrwr", "nop",
    "lqd", "stqd", "heqi", "hgti", "hlgti",
    "brz", "stqa", "brnz", "brhz", "brhnz", "stqr", "bra", "lqa", "brasl", "br", "brsl", "lqr",
    "hbra", "hbrr", "dfceq", "dfcmeq", "dfcgt", "dfcmgt", "dftsv",
}


def ops():
    out = []
    for op, m in spu_dis.RRR.items():
        out.append((m, "RRR", op))
    for op, m in spu_dis.RI18.items():
        out.append((m, "RI18", op))
    for op, m in spu_dis.RI10.items():
        out.append((m, "RI10", op))
    for op, m in spu_dis.RI16.items():
        out.append((m, "RI16", op))
    for op, m in spu_dis.RI8.items():
        out.append((m, "RI8", op))
    for op, m in spu_dis.RR.items():
        out.append((m, "RI7" if m in spu_dis.RI7_OPS else "RR", op))
    return sorted((o for o in out if o[0] not in SKIP), key=lambda o: o[0])


def rr(op, rt, ra, rb):
    return (op << 21) | (rb << 14) | (ra << 7) | rt


def ri16(op, rt, i16):
    return (op << 23) | ((i16 & 0xFFFF) << 7) | rt


def ri18(op, rt, i18):
    return (op << 25) | ((i18 & 0x3FFFF) << 7) | rt


def ri10(op, rt, ra, i10):
    return (op << 24) | ((i10 & 0x3FF) << 14) | (ra << 7) | rt


def lqa(rt, addr):
    return ri16(0x061, rt, addr >> 2)


def stqa(rt, addr):
    return ri16(0x041, rt, addr >> 2)


def il(rt, v):
    return ri16(0x081, rt, v)


def ila(rt, v):
    return ri18(0x21, rt, v)


def lqd(rt, off, ra):
    return ri10(0x34, rt, ra, off >> 4)


def stqd(rt, off, ra):
    return ri10(0x24, rt, ra, off >> 4)


def wrch(ch, rt):
    return rr(0x10D, rt, ch, 0)


def rdch(rt, ch):
    return rr(0x00D, rt, ch, 0)


def a(rt, ra, rb):
    return rr(0x0C0, rt, ra, rb)


def ai(rt, ra, i10):
    return ri10(0x1C, rt, ra, i10)


def brnz(rt, rel_words):
    return ri16(0x042, rt, rel_words)


def stop(code):
    return code & 0x3FFF


def rand_reg(r):
    return r.randrange(0, BASE_REG)


def encode(r, m, form, op):
    rt = rand_reg(r)
    ra = rand_reg(r) if r.random() < 0.85 else rt
    rb = rand_reg(r) if r.random() < 0.85 else ra
    rc = rand_reg(r)
    if form == "RRR":
        return (op << 28) | (rt << 21) | (rb << 14) | (ra << 7) | rc
    if form == "RI18":
        return ri18(op, rt, r.getrandbits(18))
    if form == "RI10":
        return ri10(op, rt, ra, r.getrandbits(10))
    if form == "RI16":
        return ri16(op, rt, r.getrandbits(16))
    if form == "RI8":
        return (op << 22) | (r.getrandbits(8) << 14) | (ra << 7) | rt
    if form == "RI7":
        return rr(op, rt, ra, r.getrandbits(7))
    return rr(op, rt, ra, rb)


F32_SPECIAL = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F7FFFFF, 0xFF7FFFFF, 0x00000001, 0x007FFFFF, 0x80400000,
               0x00800000, 0x7F800000, 0xFF800000, 0x7FC00000, 0x7FFFFFFF, 0xFFFFFFFF, 0x4F000000, 0xCF000000, 0x3F000000,
               0x4B000000, 0x4F800000, 0x5F000000, 0xDF000000, 0x00000080, 0x0000FFFF, 0x7FFF0000, 0x80000001]
F64_SPECIAL = [0x0000000000000000, 0x8000000000000000, 0x3FF0000000000000, 0xBFF0000000000000, 0x7FF0000000000000,
               0xFFF0000000000000, 0x7FF8000000000000, 0x7FF4000000000000, 0x0000000000000001, 0x000FFFFFFFFFFFFF,
               0x7FEFFFFFFFFFFFFF, 0x47EFFFFFE0000000, 0x3810000000000000, 0x36A0000000000000, 0x41DFFFFFFFC00000,
               0xC1E0000000000000, 0x43E0000000000000]


def rand_word(r):
    k = r.random()
    if k < 0.3:
        return r.choice(F32_SPECIAL)
    if k < 0.45:
        return r.randrange(0, 256)
    if k < 0.55:
        return (1 << r.randrange(32)) ^ (0xFFFFFFFF if r.random() < 0.3 else 0)
    if k < 0.75:
        e = r.randrange(100, 160)
        return (r.getrandbits(1) << 31) | (e << 23) | r.getrandbits(23)
    return r.getrandbits(32)


def rand_quad(r):
    k = r.random()
    if k < 0.15:
        hi = r.choice(F64_SPECIAL) if r.random() < 0.6 else r.getrandbits(64)
        lo = r.choice(F64_SPECIAL) if r.random() < 0.6 else r.getrandbits(64)
        return struct.pack(">QQ", hi, lo)
    if k < 0.25:
        v = rand_word(r)
        return struct.pack(">IIII", v, v, v, v)
    if k < 0.32:
        h = r.getrandbits(16)
        return struct.pack(">8H", *([h] * 8))
    return struct.pack(">IIII", *(rand_word(r) for _ in range(4)))


def make_state(r):
    s = bytearray()
    for _ in range(128):
        s += rand_quad(r)
    return bytes(s)


def dma(cmd, var):
    return [lqa(BASE_REG, VAR_LSA), wrch(16, BASE_REG), il(BASE_REG, 0), wrch(17, BASE_REG), lqa(BASE_REG, var), wrch(18, BASE_REG),
            il(BASE_REG, STATE), wrch(19, BASE_REG), il(BASE_REG, 0), wrch(20, BASE_REG), il(BASE_REG, cmd), wrch(21, BASE_REG),
            il(BASE_REG, 1), wrch(22, BASE_REG), il(BASE_REG, 2), wrch(23, BASE_REG), rdch(BASE_REG, 24)]


def block(w, states):
    code = [il(BASE_REG, states), stqa(BASE_REG, VAR_CNT)]
    loop = len(code)
    code += dma(0x40, VAR_IN)
    code.append(ila(BASE_REG, BUF))
    code += [lqd(n, n * 16, BASE_REG) for n in range(BASE_REG)]
    code.append(w)
    code += [stqd(n, n * 16, BASE_REG) for n in range(BASE_REG)]
    code += dma(0x20, VAR_OUT)
    code += [lqa(0, VAR_IN), il(1, STATE), a(0, 0, 1), stqa(0, VAR_IN)]
    code += [lqa(0, VAR_OUT), a(0, 0, 1), stqa(0, VAR_OUT)]
    code += [lqa(0, VAR_CNT), ai(0, 0, -1), stqa(0, VAR_CNT)]
    code.append(brnz(0, loop - len(code)))
    return code


def program(words, states):
    code = [stqa(3, VAR_IN), stqa(4, VAR_OUT), ila(BASE_REG, BUF), stqa(BASE_REG, VAR_LSA)]
    for w in words:
        code += block(w, states)
    code += [il(0, 0), wrch(28, 0), stop(0x102)]
    while len(code) % 4:
        code.append(stop(0x102))
    return b"".join(struct.pack(">I", c) for c in code)


def ppu_code(images, d):
    c = []
    for img in images:
        c += asm.load_imm32(3, d["gid"]) + asm.load_imm32(6, d["gattr"])
        c += [asm.li(4, 1), asm.li(5, 100), asm.li(11, 170), asm.sc()]
        c += asm.load_imm32(9, d["gid"]) + [asm.lwz(4, 0, 9)]
        c += asm.load_imm32(3, d["tid"]) + asm.load_imm32(6, img["img"]) + asm.load_imm32(7, d["tattr"]) + asm.load_imm32(8, img["arg"])
        c += [asm.li(5, 0), asm.li(11, 172), asm.sc()]
        c += asm.load_imm32(9, d["gid"]) + [asm.lwz(3, 0, 9), asm.li(11, 173), asm.sc()]
        c += asm.load_imm32(9, d["gid"]) + [asm.lwz(3, 0, 9)]
        c += asm.load_imm32(4, d["cause"]) + asm.load_imm32(5, d["status"]) + [asm.li(11, 178), asm.sc()]
        c += asm.load_imm32(9, d["gid"]) + [asm.lwz(3, 0, 9), asm.li(11, 171), asm.sc()]
    c += asm.load_imm32(3, d["path"]) + [asm.li(4, 0x241)] + asm.load_imm32(5, d["fd"])
    c += [asm.li(6, 0x1B6), asm.li(7, 0), asm.li(8, 0), asm.li(11, 801), asm.sc()]
    c += asm.load_imm32(9, d["fd"]) + [asm.lwz(3, 0, 9)]
    c += asm.load_imm32(4, d["out"]) + asm.load_imm32(5, d["out_size"]) + asm.load_imm32(6, d["nw"])
    c += [asm.li(11, 803), asm.sc()]
    c += asm.load_imm32(9, d["fd"]) + [asm.lwz(3, 0, 9), asm.li(11, 804), asm.sc()]
    c += [asm.li(3, 0), asm.li(4, 0), asm.li(5, 0), asm.li(11, 22), asm.sc(), asm.b(0)]
    return c


def build_elf(segment, memsz, entry):
    phoff = 0x40
    seg_off = 0x10000
    eh = bytearray(64)
    eh[0:16] = b"\x7fELF" + bytes([2, 2, 1, 0x66, 0]) + bytes(7)
    struct.pack_into(">HHIQQQIHHHHHH", eh, 16, 2, 21, 1, entry, phoff, 0, 0, 64, 56, 1, 64, 0, 0)
    ph = struct.pack(">IIQQQQQQ", 1, 7, seg_off, LOAD_BASE, LOAD_BASE, len(segment), memsz, 0x10000)
    out = bytearray(seg_off)
    out[0:64] = eh
    out[phoff:phoff + 56] = ph
    return bytes(out) + segment


def align(v, n):
    return (v + n - 1) & ~(n - 1)


def main(out_dir, per_mnemonic="6", states="4", seed="1"):
    per_mnemonic, states = int(per_mnemonic), int(states)
    r = random.Random(int(seed))
    tests = []
    for m, form, op in ops():
        seen = set()
        for _ in range(per_mnemonic * 4):
            if len(seen) >= per_mnemonic:
                break
            w = encode(r, m, form, op)
            if w not in seen:
                seen.add(w)
                tests.append({"m": m, "w": w})
    block_bytes = len(block(0, states)) * 4
    per_image = (CODE_LIMIT - 64) // block_bytes
    groups = [tests[i:i + per_image] for i in range(0, len(tests), per_image)]
    programs = [program([t["w"] for t in g], states) for g in groups]
    nstates = len(tests) * states
    data = {}
    cur = LOAD_BASE + 16
    ppu_words = len(ppu_code([{"img": 0, "arg": 0}] * len(groups), {k: 0 for k in ("gid", "gattr", "tid", "tattr", "cause", "status", "path", "fd", "out", "out_size", "nw")}))
    code_addr = cur
    cur = align(cur + ppu_words * 4, 0x100)
    for k in ("gid", "tid", "cause", "status", "fd", "nw"):
        data[k] = cur
        cur += 16
    data["gattr"] = cur
    cur += 16
    data["tattr"] = cur
    cur += 16
    name_addr = cur
    cur += 16
    path = b"/dev_hdd0/tmp/spu_conformance.bin\0"
    data["path"] = cur
    cur = align(cur + len(path), 0x80)
    images = []
    for g, prog in zip(groups, programs):
        images.append({"img": cur, "seg": cur + 0x20, "arg": cur + 0x40})
        cur += 0x80
    for im, prog in zip(images, programs):
        im["code"] = cur
        im["size"] = len(prog)
        cur = align(cur + len(prog), 0x80)
    inputs_addr = cur
    cur = align(cur + nstates * STATE, 0x10000)
    out_addr = cur
    data["out"] = out_addr
    data["out_size"] = nstates * STATE
    memsz = out_addr + nstates * STATE - LOAD_BASE
    seg = bytearray(inputs_addr - LOAD_BASE)
    code = ppu_code(images, data)
    struct.pack_into(">II", seg, 0, code_addr, 0)
    for i, w in enumerate(code):
        struct.pack_into(">I", seg, code_addr - LOAD_BASE + 4 * i, w)
    struct.pack_into(">IIii", seg, data["gattr"] - LOAD_BASE, 5, name_addr, 0, 0)
    struct.pack_into(">III", seg, data["tattr"] - LOAD_BASE, name_addr, 5, 0)
    seg[name_addr - LOAD_BASE:name_addr - LOAD_BASE + 5] = b"conf\0"
    seg[data["path"] - LOAD_BASE:data["path"] - LOAD_BASE + len(path)] = path
    first = 0
    for g, im, prog in zip(groups, images, programs):
        struct.pack_into(">IIIi", seg, im["img"] - LOAD_BASE, 0, 0, im["seg"], 1)
        struct.pack_into(">iIIIQ", seg, im["seg"] - LOAD_BASE, 1, 0, im["size"], 0, 0)
        struct.pack_into(">I", seg, im["seg"] - LOAD_BASE + 16, im["code"])
        in_ea = inputs_addr + first * states * STATE
        out_ea = out_addr + first * states * STATE
        struct.pack_into(">QQQQ", seg, im["arg"] - LOAD_BASE, in_ea << 32, out_ea << 32, 0, 0)
        seg[im["code"] - LOAD_BASE:im["code"] - LOAD_BASE + len(prog)] = prog
        first += len(g)
    inputs = bytearray()
    for t in tests:
        for _ in range(states):
            inputs += make_state(r)
    seg += inputs
    os.makedirs(out_dir, exist_ok=True)
    open(os.path.join(out_dir, "spu_conformance.elf"), "wb").write(build_elf(bytes(seg), memsz, LOAD_BASE))
    open(os.path.join(out_dir, "inputs.bin"), "wb").write(inputs)
    json.dump({"states": states, "state_size": STATE, "tests": tests}, open(os.path.join(out_dir, "tests.json"), "w"), indent=0)
    with open(os.path.join(out_dir, "tests.txt"), "w") as f:
        f.write(f"{states}\n")
        for t in tests:
            f.write(f"{t['m']} {t['w']:08x}\n")
    mnems = len({t["m"] for t in tests})
    print(f"{mnems} mnemonics, {len(tests)} encodings, {nstates} states, {len(groups)} SPU images, elf memsz 0x{memsz:x}")


if __name__ == "__main__":
    main(*sys.argv[1:])
