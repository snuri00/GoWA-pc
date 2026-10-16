import json
import math
import os
import random
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "recomp", "ppu"))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))

import asm
from analysis import Image, discover
from codegen import gen_insn, Fields
from isa import mnemonic

SLOT = 0x440
OFF_GPR, OFF_FPR, OFF_VR, OFF_CR, OFF_XER, OFF_CTR, OFF_LR, OFF_FPSCR, OFF_VSCR = 0x0, 0x100, 0x200, 0x400, 0x408, 0x410, 0x418, 0x420, 0x430
LOAD_BASE = 0x10000
SPR_OK = {1, 8, 9}

EXCLUDE = {
    "b", "bc", "bclr", "bcctr", "sc", "tw", "twi", "td", "tdi", "sync", "eieio", "isync", "rfi",
    "dcbt", "dcbtst", "dcbst", "dcbf", "dcbz", "icbi", "mftb", "mfmsr", "mffs", "lwarx", "ldarx", "stwcx.", "stdcx.",
}


def testable(m, w):
    if m is None or m in EXCLUDE:
        return False
    if m in ("lvsl", "lvsr"):
        return True
    if m.startswith(("l", "st")) and m not in ("lvsl", "lvsr"):
        return False
    if m in ("mfspr", "mtspr"):
        return Fields(w).spr in SPR_OK
    return True


def used_gprs(w):
    return {(w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31}


GPR_SPECIAL = [0, 1, 2, 31, 32, 63, 64, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
               0x100000000, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFF80000000]
F64_SPECIAL = [0.0, -0.0, 1.0, -1.0, 0.5, 1.5, -2.75, 3.4e38, 3.5e38, 1e300, -1e300, 1e-310, 1.17549435e-38, 1e-45,
               2147483647.5, -2147483648.5, 4294967296.0, 9.3e18, float("inf"), float("-inf")]
F64_NAN_BITS = [0x7FF8000000000000, 0x7FF4000000000000, 0xFFF8000000000001]
F32_SPECIAL_BITS = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F7FFFFF, 0x00000001, 0x007FFFFF, 0x80400000,
                    0x7F800000, 0xFF800000, 0x7FC00000, 0x7FA00000, 0xFFC00001, 0x4F000000, 0xCF000000, 0x3F000000]


def rand_gpr(r):
    k = r.random()
    if k < 0.35:
        return r.choice(GPR_SPECIAL)
    if k < 0.55:
        return r.randrange(0, 128)
    if k < 0.75:
        v = r.getrandbits(32)
        return v | (0xFFFFFFFF00000000 if v & 0x80000000 and r.random() < 0.5 else 0)
    return r.getrandbits(64)


def rand_f64_bits(r):
    k = r.random()
    if k < 0.3:
        return struct.unpack(">Q", struct.pack(">d", r.choice(F64_SPECIAL)))[0]
    if k < 0.36:
        return r.choice(F64_NAN_BITS)
    if k < 0.7:
        f = struct.unpack(">f", struct.pack(">I", r.getrandbits(32) & 0x7F7FFFFF | (r.getrandbits(1) << 31)))[0]
        return struct.unpack(">Q", struct.pack(">d", f))[0]
    if k < 0.85:
        v = r.uniform(-1e6, 1e6)
        return struct.unpack(">Q", struct.pack(">d", v))[0]
    return r.getrandbits(64)


def rand_v128(r):
    k = r.random()
    if k < 0.45:
        words = [r.choice(F32_SPECIAL_BITS) if r.random() < 0.5 else struct.unpack(">I", struct.pack(">f", r.uniform(-1000, 1000)))[0] for _ in range(4)]
        return b"".join(struct.pack(">I", w) for w in words)
    if k < 0.6:
        return bytes(r.choice([0, 1, 0x7F, 0x80, 0xFF, r.getrandbits(8)]) for _ in range(16))
    if k < 0.7:
        return bytes([r.randrange(0, 32) for _ in range(16)])
    return r.getrandbits(128).to_bytes(16, "big")


def make_state(r):
    s = bytearray(SLOT)
    for n in range(32):
        struct.pack_into(">Q", s, OFF_GPR + 8 * n, rand_gpr(r))
        struct.pack_into(">Q", s, OFF_FPR + 8 * n, rand_f64_bits(r))
        s[OFF_VR + 16 * n:OFF_VR + 16 * n + 16] = rand_v128(r)
    struct.pack_into(">I", s, OFF_CR, r.getrandbits(32))
    struct.pack_into(">Q", s, OFF_XER, (r.getrandbits(3) << 29) | r.randrange(0, 128))
    struct.pack_into(">Q", s, OFF_CTR, rand_gpr(r))
    struct.pack_into(">Q", s, OFF_LR, rand_gpr(r))
    struct.pack_into(">Q", s, OFF_FPSCR, 0)
    vscr = 0x10000 if r.random() < 0.85 else 0
    for k in range(4):
        struct.pack_into(">I", s, OFF_VSCR + 4 * k, vscr)
    return bytes(s)


def harness_block(w, rb, slot_addr, count, cnt_addr, here):
    rx = next(n for n in (3, 4, 5) if n != rb)
    ry, rz = [n for n in (6, 7, 8, 9) if n != rb][:2]
    code = []
    code += asm.load_imm32(rb, slot_addr)
    code += asm.load_imm32(ry, cnt_addr)
    code += [asm.li(rz, count), asm.stw(rz, 0, ry)]
    loop = len(code)
    code += [asm.li(rx, OFF_VSCR), asm.lvx(0, rb, rx), asm.mtvscr(0)]
    code += [asm.lfd(0, OFF_FPSCR, rb), asm.mtfsf(0xFF, 0)]
    for n in range(32):
        code += [asm.li(rx, OFF_VR + 16 * n), asm.lvx(n, rb, rx)]
    for n in range(32):
        code.append(asm.lfd(n, OFF_FPR + 8 * n, rb))
    code += [asm.lwz(rx, OFF_CR, rb), asm.mtcrf(0xFF, rx)]
    code += [asm.ld(rx, OFF_XER, rb), asm.mtspr(1, rx)]
    code += [asm.ld(rx, OFF_CTR, rb), asm.mtspr(9, rx)]
    code += [asm.ld(rx, OFF_LR, rb), asm.mtspr(8, rx)]
    for n in range(32):
        if n != rb:
            code.append(asm.ld(n, OFF_GPR + 8 * n, rb))
    code.append(w)
    for n in range(32):
        if n != rb:
            code.append(asm.std(n, OFF_GPR + 8 * n, rb))
    code += [asm.mfcr(rx), asm.stw(rx, OFF_CR, rb)]
    code += [asm.mfspr(rx, 1), asm.std(rx, OFF_XER, rb)]
    code += [asm.mfspr(rx, 9), asm.std(rx, OFF_CTR, rb)]
    code += [asm.mfspr(rx, 8), asm.std(rx, OFF_LR, rb)]
    for n in range(32):
        code.append(asm.stfd(n, OFF_FPR + 8 * n, rb))
    code += [asm.mffs(0), asm.stfd(0, OFF_FPSCR, rb)]
    for n in range(32):
        code += [asm.li(rx, OFF_VR + 16 * n), asm.stvx(n, rb, rx)]
    code += [asm.mfvscr(0), asm.li(rx, OFF_VSCR), asm.stvx(0, rb, rx)]
    code.append(asm.addi(rb, rb, SLOT))
    code += asm.load_imm32(ry, cnt_addr)
    code += [asm.lwz(rz, 0, ry), asm.addic_(rz, rz, -1), asm.stw(rz, 0, ry)]
    code.append(asm.bne((loop - len(code)) * 4))
    return code


def epilogue(path_addr, fd_addr, nw_addr, slots_addr, slots_size):
    c = []
    c += asm.load_imm32(3, path_addr)
    c += [asm.li(4, 0x241)]
    c += asm.load_imm32(5, fd_addr)
    c += [asm.li(6, 0x1B6), asm.li(7, 0), asm.li(8, 0), asm.li(11, 801), asm.sc()]
    c += asm.load_imm32(9, fd_addr)
    c += [asm.lwz(3, 0, 9)]
    c += asm.load_imm32(4, slots_addr)
    c += asm.load_imm32(5, slots_size)
    c += asm.load_imm32(6, nw_addr)
    c += [asm.li(11, 803), asm.sc()]
    c += asm.load_imm32(9, fd_addr)
    c += [asm.lwz(3, 0, 9), asm.li(11, 804), asm.sc()]
    c += [asm.li(3, 0), asm.li(4, 0), asm.li(5, 0), asm.li(11, 22), asm.sc()]
    c += [asm.b(0)]
    return c


def build_elf(segment, entry):
    phoff = 0x40
    seg_off = 0x10000
    eh = bytearray(64)
    eh[0:16] = b"\x7fELF" + bytes([2, 2, 1, 0x66, 0]) + bytes(7)
    struct.pack_into(">HHIQQQIHHHHHH", eh, 16, 2, 21, 1, entry, phoff, 0, 0, 64, 56, 1, 64, 0, 0)
    ph = struct.pack(">IIQQQQQQ", 1, 7, seg_off, LOAD_BASE, LOAD_BASE, len(segment), len(segment), 0x10000)
    out = bytearray(seg_off)
    out[0:64] = eh
    out[phoff:phoff + 56] = ph
    return bytes(out) + segment


def main(elf_path, out_dir, per_mnemonic="6", states="4", seed="1"):
    per_mnemonic, states = int(per_mnemonic), int(states)
    r = random.Random(int(seed))
    img = Image(elf_path)
    funcs, _ = discover(img)
    reached = sorted(set().union(*(f.insns for f in funcs.values())))
    by_m = {}
    for pc in reached:
        w = img.word(pc)
        m = mnemonic(w)
        if testable(m, w) and gen_insn(m, w) is not None:
            by_m.setdefault(m, set()).add(w)
    tests = []
    for m in sorted(by_m):
        words = sorted(by_m[m])
        r.shuffle(words)
        for w in words[:per_mnemonic]:
            rb = next(n for n in (31, 30, 29, 28, 27, 26) if n not in used_gprs(w))
            tests.append({"m": m, "w": w, "rb": rb})
    nslots = len(tests) * states
    blocks = []
    for t in tests:
        blocks.append(len(harness_block(t["w"], t["rb"], 0, states, 0, 0)))
    opd_size = 16
    code_words = sum(blocks) + len(epilogue(0, 0, 0, 0, 0))
    code_addr = LOAD_BASE + opd_size
    data_addr = (code_addr + code_words * 4 + 0xFF) & ~0xFF
    cnt_addr = data_addr
    fd_addr = data_addr + 8
    nw_addr = data_addr + 16
    path_addr = data_addr + 32
    path = b"/dev_hdd0/tmp/ppu_conformance.bin\0"
    slots_addr = (path_addr + len(path) + 0xFF) & ~0xFF
    code = []
    inputs = bytearray()
    for k, t in enumerate(tests):
        t["slot"] = k * states
        code += harness_block(t["w"], t["rb"], slots_addr + k * states * SLOT, states, cnt_addr, 0)
        for _ in range(states):
            inputs += make_state(r)
    code += epilogue(path_addr, fd_addr, nw_addr, slots_addr, nslots * SLOT)
    seg = bytearray(slots_addr - LOAD_BASE + len(inputs))
    struct.pack_into(">II", seg, 0, code_addr, 0)
    for i, w in enumerate(code):
        struct.pack_into(">I", seg, code_addr - LOAD_BASE + 4 * i, w)
    seg[path_addr - LOAD_BASE:path_addr - LOAD_BASE + len(path)] = path
    seg[slots_addr - LOAD_BASE:] = inputs
    os.makedirs(out_dir, exist_ok=True)
    open(os.path.join(out_dir, "ppu_conformance.elf"), "wb").write(build_elf(bytes(seg), LOAD_BASE))
    open(os.path.join(out_dir, "inputs.bin"), "wb").write(inputs)
    json.dump({"states": states, "slot": SLOT, "tests": tests}, open(os.path.join(out_dir, "tests.json"), "w"), indent=0)
    with open(os.path.join(out_dir, "tests.cpp"), "w") as f:
        f.write('#include "ppu/ops.h"\n#include "conformance.h"\n\n')
        for k, t in enumerate(tests):
            body = gen_insn(t["m"], t["w"]).replace("PC", "0")
            f.write(f"static void t_{k}(ppu::Context& ctx, uint8_t* base) {{ {body} }}\n")
        f.write("\nconst ConformanceTest conformance_tests[] = {\n")
        for k, t in enumerate(tests):
            f.write(f'    {{"{t["m"]}", 0x{t["w"]:08x}u, {t["rb"]}, t_{k}}},\n')
        f.write(f"}};\nconst uint32_t conformance_test_count = {len(tests)};\nconst uint32_t conformance_states = {states};\n")
    print(f"{len(by_m)} mnemonics, {len(tests)} encodings, {nslots} states, code {code_words * 4} bytes, elf {len(seg) + 0x10000} bytes")


if __name__ == "__main__":
    main(*sys.argv[1:])
