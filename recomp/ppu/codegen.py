import os
import sys

from analysis import Image, discover, islands, looks_like_function, branch_target, bo_always, sext
from isa import mnemonic


def G(n):
    return f"ctx.gpr[{n}]"


def F(n):
    return f"ctx.fpr[{n}]"


def V(n):
    return f"ctx.vr[{n}]"


def RA0(n):
    return "0" if n == 0 else G(n)


class Fields:
    def __init__(self, w):
        self.w = w
        self.rt = (w >> 21) & 31
        self.ra = (w >> 16) & 31
        self.rb = (w >> 11) & 31
        self.rc = (w >> 6) & 31
        self.rec = w & 1
        self.oe = (w >> 10) & 1
        self.simm = sext(w & 0xFFFF, 16)
        self.uimm = w & 0xFFFF
        self.ds = sext(w & 0xFFFC, 16)
        self.mb = (w >> 6) & 31
        self.me = (w >> 1) & 31
        self.md_sh = ((w >> 11) & 31) | (((w >> 1) & 1) << 5)
        self.md_mb = ((w >> 6) & 31) | (((w >> 5) & 1) << 5)
        self.spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5)
        self.crfd = (w >> 23) & 7
        self.crfs = (w >> 18) & 7
        self.l = (w >> 21) & 1
        self.bo = (w >> 21) & 31
        self.bi = (w >> 16) & 31


def rec(f, dst):
    return f" ppu::cr0(ctx, {dst});" if f.rec else ""


def ea_d(f):
    return f"uint32_t({RA0(f.ra)} + {f.simm})"


def ea_ds(f):
    return f"uint32_t({RA0(f.ra)} + {f.ds})"


def ea_x(f):
    return f"uint32_t({RA0(f.ra)} + {G(f.rb)})"


def ea_du(f):
    return f"uint32_t({G(f.ra)} + {f.simm})"


def ea_xu(f):
    return f"uint32_t({G(f.ra)} + {G(f.rb)})"


INT_LOADS = {
    "lbz": ("ld8", "uint64_t", 1), "lhz": ("ld16", "uint64_t", 2), "lha": ("ld16", "int16_t", 2),
    "lwz": ("ld32", "uint64_t", 4), "lwa": ("ld32", "int32_t", 4), "ld": ("ld64", "uint64_t", 8),
}
INT_STORES = {"stb": ("st8", "uint8_t"), "sth": ("st16", "uint16_t"), "stw": ("st32", "uint32_t"), "std": ("st64", "uint64_t")}


def load_expr(fn, cast, ea):
    if cast == "uint64_t":
        return f"uint64_t(ppu::{fn}(base, {ea}))"
    return f"uint64_t(int64_t({cast}(ppu::{fn}(base, {ea}))))"


def gen_int(m, f):
    rt, ra, rb = f.rt, f.ra, f.rb
    if m in ("lbz", "lhz", "lha", "lwz"):
        fn, cast, _ = INT_LOADS[m]
        return f"{G(rt)} = {load_expr(fn, cast, ea_d(f))};"
    if m in ("lbzu", "lhzu", "lhau", "lwzu"):
        fn, cast, _ = INT_LOADS[m[:-1]]
        return f"{{ uint32_t ea = {ea_du(f)}; {G(rt)} = {load_expr(fn, cast, 'ea')}; {G(ra)} = {G(ra)} + {f.simm}; }}"
    if m in ("lbzx", "lhzx", "lhax", "lwzx", "lwax", "ldx"):
        fn, cast, _ = INT_LOADS[m[:-1]]
        return f"{G(rt)} = {load_expr(fn, cast, ea_x(f))};"
    if m in ("lbzux", "lhzux", "lhaux", "lwzux", "lwaux", "ldux"):
        fn, cast, _ = INT_LOADS[m[:-2]]
        return f"{{ uint32_t ea = {ea_xu(f)}; {G(rt)} = {load_expr(fn, cast, 'ea')}; {G(ra)} = {G(ra)} + {G(rb)}; }}"
    if m in ("ld", "lwa"):
        fn, cast, _ = INT_LOADS[m]
        return f"{G(rt)} = {load_expr(fn, cast, ea_ds(f))};"
    if m == "ldu":
        return f"{{ uint32_t ea = uint32_t({G(ra)} + {f.ds}); {G(rt)} = ppu::ld64(base, ea); {G(ra)} = {G(ra)} + {f.ds}; }}"
    if m in ("stb", "sth", "stw"):
        fn, cast = INT_STORES[m]
        return f"ppu::{fn}(base, {ea_d(f)}, {cast}({G(rt)}));"
    if m in ("stbu", "sthu", "stwu"):
        fn, cast = INT_STORES[m[:-1]]
        return f"{{ uint32_t ea = {ea_du(f)}; ppu::{fn}(base, ea, {cast}({G(rt)})); {G(ra)} = {G(ra)} + {f.simm}; }}"
    if m in ("stbx", "sthx", "stwx", "stdx"):
        fn, cast = INT_STORES[m[:-1]]
        return f"ppu::{fn}(base, {ea_x(f)}, {cast}({G(rt)}));"
    if m in ("stbux", "sthux", "stwux", "stdux"):
        fn, cast = INT_STORES[m[:-2]]
        return f"{{ uint32_t ea = {ea_xu(f)}; ppu::{fn}(base, ea, {cast}({G(rt)})); {G(ra)} = {G(ra)} + {G(rb)}; }}"
    if m == "std":
        return f"ppu::st64(base, {ea_ds(f)}, {G(rt)});"
    if m == "stdu":
        return f"{{ uint32_t ea = uint32_t({G(ra)} + {f.ds}); ppu::st64(base, ea, {G(rt)}); {G(ra)} = {G(ra)} + {f.ds}; }}"
    if m == "lhbrx":
        return f"{G(rt)} = __builtin_bswap16(ppu::ld16(base, {ea_x(f)}));"
    if m == "lwbrx":
        return f"{G(rt)} = __builtin_bswap32(ppu::ld32(base, {ea_x(f)}));"
    if m == "ldbrx":
        return f"{G(rt)} = __builtin_bswap64(ppu::ld64(base, {ea_x(f)}));"
    if m == "sthbrx":
        return f"ppu::st16(base, {ea_x(f)}, __builtin_bswap16(uint16_t({G(rt)})));"
    if m == "stwbrx":
        return f"ppu::st32(base, {ea_x(f)}, __builtin_bswap32(uint32_t({G(rt)})));"
    if m == "stdbrx":
        return f"ppu::st64(base, {ea_x(f)}, __builtin_bswap64({G(rt)}));"
    if m == "lmw":
        return f"{{ uint32_t ea = {ea_d(f)}; for (int i = {rt}; i < 32; i++, ea += 4) ctx.gpr[i] = ppu::ld32(base, ea); }}"
    if m == "stmw":
        return f"{{ uint32_t ea = {ea_d(f)}; for (int i = {rt}; i < 32; i++, ea += 4) ppu::st32(base, ea, uint32_t(ctx.gpr[i])); }}"
    if m == "lwarx":
        return f"{G(rt)} = ppu::lwarx(ctx, base, {ea_x(f)});"
    if m == "ldarx":
        return f"{G(rt)} = ppu::ldarx(ctx, base, {ea_x(f)});"
    if m == "stwcx.":
        return f"ctx.cr[0] = {{0, 0, uint8_t(ppu::stwcx(ctx, base, {ea_x(f)}, uint32_t({G(rt)}))), ctx.xer_so}};"
    if m == "stdcx.":
        return f"ctx.cr[0] = {{0, 0, uint8_t(ppu::stdcx(ctx, base, {ea_x(f)}, {G(rt)})), ctx.xer_so}};"

    if m == "addi":
        return f"{G(rt)} = {RA0(ra)} + {f.simm};"
    if m == "addis":
        return f"{G(rt)} = {RA0(ra)} + {f.simm << 16};"
    if m in ("addic", "addic."):
        s = f"{{ uint64_t a = {G(ra)}; uint64_t r = a + {f.simm}; ctx.xer_ca = r < a; {G(rt)} = r; }}"
        if m == "addic.":
            s += f" ppu::cr0(ctx, {G(rt)});"
        return s
    if m == "subfic":
        return f"{{ uint64_t a = {G(ra)}; uint64_t r = uint64_t({f.simm}) - a; ctx.xer_ca = a <= uint64_t({f.simm}); {G(rt)} = r; }}"
    if m == "mulli":
        return f"{G(rt)} = uint64_t(int64_t({G(ra)}) * {f.simm});"
    if m in ("ori", "oris", "xori", "xoris", "andi.", "andis."):
        imm = f.uimm << 16 if m.endswith("is") or m == "andis." else f.uimm
        op = {"o": "|", "x": "^", "a": "&"}[m[0]]
        s = f"{G(ra)} = {G(rt)} {op} 0x{imm:x}ull;"
        if m.startswith("and"):
            s += f" ppu::cr0(ctx, {G(ra)});"
        return s
    if m in ("cmpi", "cmpli", "cmp", "cmpl"):
        if m == "cmpi":
            a, b, fn = (f"int64_t({G(ra)})", str(f.simm), "cr_set") if f.l else (f"int64_t(int32_t({G(ra)}))", str(f.simm), "cr_set")
        elif m == "cmpli":
            a, b, fn = (G(ra), f"0x{f.uimm:x}ull", "cr_setu") if f.l else (f"uint64_t(uint32_t({G(ra)}))", f"0x{f.uimm:x}ull", "cr_setu")
        elif m == "cmp":
            a, b, fn = (f"int64_t({G(ra)})", f"int64_t({G(rb)})", "cr_set") if f.l else (f"int64_t(int32_t({G(ra)}))", f"int64_t(int32_t({G(rb)}))", "cr_set")
        else:
            a, b, fn = (G(ra), G(rb), "cr_setu") if f.l else (f"uint64_t(uint32_t({G(ra)}))", f"uint64_t(uint32_t({G(rb)}))", "cr_setu")
        return f"ppu::{fn}(ctx, {f.crfd}, {a}, {b});"

    base_m = m[:-1] if m.endswith("o") and m[:-1] in ("add", "addc", "adde", "addze", "addme", "subf", "subfc", "subfe", "subfze", "subfme", "neg", "mullw", "mulld", "divw", "divwu", "divd", "divdu") else m
    oe = base_m != m
    if base_m in ("add", "subf", "neg", "addc", "subfc", "adde", "subfe", "addze", "subfze", "addme", "subfme"):
        a, b = G(ra), G(rb)
        parts = {
            "add": (a, b, "0", False), "subf": (f"~{a}", b, "1", False), "neg": (f"~{a}", "0", "1", False),
            "addc": (a, b, "0", True), "subfc": (f"~{a}", b, "1", True), "adde": (a, b, "ctx.xer_ca", True),
            "subfe": (f"~{a}", b, "ctx.xer_ca", True), "addze": (a, "0", "ctx.xer_ca", True),
            "subfze": (f"~{a}", "0", "ctx.xer_ca", True), "addme": (a, "~0ull", "ctx.xer_ca", True),
            "subfme": (f"~{a}", "~0ull", "ctx.xer_ca", True),
        }[base_m]
        x, y, ci, ca = parts
        s = f"{{ uint64_t x = {x}; uint64_t y = {y}; uint8_t co; uint64_t r = ppu::add_ca(x, y, {ci}, co);"
        if ca:
            s += " ctx.xer_ca = co;"
        if oe:
            s += " ppu::set_ov(ctx, ppu::add_ov(x, y, r));"
        s += f" {G(rt)} = r; }}" + rec(f, G(rt))
        return s
    if base_m in ("mullw", "mulld", "divw", "divwu", "divd", "divdu"):
        expr = {
            "mullw": f"uint64_t(int64_t(int32_t({G(ra)})) * int64_t(int32_t({G(rb)})))",
            "mulld": f"{G(ra)} * {G(rb)}",
            "divw": f"ppu::divw({G(ra)}, {G(rb)})", "divwu": f"ppu::divwu({G(ra)}, {G(rb)})",
            "divd": f"ppu::divd({G(ra)}, {G(rb)})", "divdu": f"ppu::divdu({G(ra)}, {G(rb)})",
        }[base_m]
        s = f"{G(rt)} = {expr};"
        if oe:
            ov = {
                "mullw": f"int64_t({G(rt)}) != int64_t(int32_t({G(rt)}))",
                "mulld": "false",
                "divw": f"(int32_t({G(rb)}) == 0 || (int32_t({G(ra)}) == INT32_MIN && int32_t({G(rb)}) == -1))",
                "divwu": f"uint32_t({G(rb)}) == 0",
                "divd": f"({G(rb)} == 0 || (int64_t({G(ra)}) == INT64_MIN && int64_t({G(rb)}) == -1))",
                "divdu": f"{G(rb)} == 0",
            }[base_m]
            if base_m.startswith("div"):
                s = f"{{ bool ov = {ov}; {G(rt)} = {expr}; ppu::set_ov(ctx, ov); }}"
            else:
                s += f" ppu::set_ov(ctx, {ov});"
        return s + rec(f, G(rt))
    if m in ("mulhw", "mulhwu", "mulhd", "mulhdu"):
        return f"{G(rt)} = ppu::{m}({G(ra)}, {G(rb)});" + rec(f, G(rt))

    logic = {
        "and": f"{G(rt)} & {G(rb)}", "andc": f"{G(rt)} & ~{G(rb)}", "or": f"{G(rt)} | {G(rb)}",
        "orc": f"{G(rt)} | ~{G(rb)}", "xor": f"{G(rt)} ^ {G(rb)}", "nand": f"~({G(rt)} & {G(rb)})",
        "nor": f"~({G(rt)} | {G(rb)})", "eqv": f"~({G(rt)} ^ {G(rb)})",
        "extsb": f"uint64_t(int64_t(int8_t({G(rt)})))", "extsh": f"uint64_t(int64_t(int16_t({G(rt)})))",
        "extsw": f"uint64_t(int64_t(int32_t({G(rt)})))",
        "cntlzw": f"uint64_t(std::countl_zero(uint32_t({G(rt)})))", "cntlzd": f"uint64_t(std::countl_zero({G(rt)}))",
        "slw": f"ppu::slw({G(rt)}, {G(rb)})", "srw": f"ppu::srw({G(rt)}, {G(rb)})",
        "sld": f"ppu::sld({G(rt)}, {G(rb)})", "srd": f"ppu::srd({G(rt)}, {G(rb)})",
        "sraw": f"ppu::sraw(ctx, {G(rt)}, {G(rb)})", "srad": f"ppu::srad(ctx, {G(rt)}, {G(rb)})",
    }
    if m in logic:
        return f"{G(ra)} = {logic[m]};" + rec(f, G(ra))
    if m == "srawi":
        sh = f.rb
        return (f"{{ int32_t v = int32_t({G(rt)}); ctx.xer_ca = v < 0 && (uint32_t(v) & 0x{(1 << sh) - 1:x}u) != 0; "
                f"{G(ra)} = uint64_t(int64_t(v >> {sh})); }}") + rec(f, G(ra))
    if m == "sradi":
        sh = f.md_sh
        mask = (1 << sh) - 1
        return (f"{{ int64_t v = int64_t({G(rt)}); ctx.xer_ca = v < 0 && (uint64_t(v) & 0x{mask:x}ull) != 0; "
                f"{G(ra)} = uint64_t(v >> {sh}); }}") + rec(f, G(ra))
    if m == "rlwinm":
        mask = f"0x{mask64(f.mb + 32, f.me + 32):x}ull"
        return f"{G(ra)} = ppu::rotl32({G(rt)}, {f.rb}) & {mask};" + rec(f, G(ra))
    if m == "rlwnm":
        mask = f"0x{mask64(f.mb + 32, f.me + 32):x}ull"
        return f"{G(ra)} = ppu::rotl32({G(rt)}, uint32_t({G(rb)}) & 31) & {mask};" + rec(f, G(ra))
    if m == "rlwimi":
        mk = mask64(f.mb + 32, f.me + 32)
        return f"{G(ra)} = (ppu::rotl32({G(rt)}, {f.rb}) & 0x{mk:x}ull) | ({G(ra)} & 0x{(~mk) & 0xFFFFFFFFFFFFFFFF:x}ull);" + rec(f, G(ra))
    if m in ("rldicl", "rldicr", "rldic", "rldimi"):
        sh, mb = f.md_sh, f.md_mb
        if m == "rldicl":
            mk = mask64(mb, 63)
        elif m == "rldicr":
            mk = mask64(0, mb)
        else:
            mk = mask64(mb, 63 - sh)
        if m == "rldimi":
            return f"{G(ra)} = (ppu::rotl64({G(rt)}, {sh}) & 0x{mk:x}ull) | ({G(ra)} & 0x{(~mk) & 0xFFFFFFFFFFFFFFFF:x}ull);" + rec(f, G(ra))
        return f"{G(ra)} = ppu::rotl64({G(rt)}, {sh}) & 0x{mk:x}ull;" + rec(f, G(ra))
    if m in ("rldcl", "rldcr"):
        mb = f.md_mb
        mk = mask64(mb, 63) if m == "rldcl" else mask64(0, mb)
        return f"{G(ra)} = ppu::rotl64({G(rt)}, uint32_t({G(rb)}) & 63) & 0x{mk:x}ull;" + rec(f, G(ra))

    if m == "mfspr":
        if f.spr == 8:
            return f"{G(rt)} = ctx.lr;"
        if f.spr == 9:
            return f"{G(rt)} = ctx.ctr;"
        if f.spr == 1:
            return f"{G(rt)} = ppu::xer_get(ctx);"
        if f.spr == 256:
            return f"{G(rt)} = 0;"
        if f.spr in (268, 269):
            return f"{G(rt)} = {'ppu::timebase()' if f.spr == 268 else 'ppu::timebase() >> 32'};"
        return None
    if m == "mtspr":
        if f.spr == 8:
            return f"ctx.lr = {G(rt)};"
        if f.spr == 9:
            return f"ctx.ctr = {G(rt)};"
        if f.spr == 1:
            return f"ppu::xer_put(ctx, {G(rt)});"
        if f.spr == 256:
            return ";"
        return None
    if m == "mftb":
        return f"{G(rt)} = {'ppu::timebase()' if f.spr == 268 else 'ppu::timebase() >> 32'};"
    if m == "mfcr":
        if (f.w >> 20) & 1:
            fxm = (f.w >> 12) & 0xFF
            mk = 0
            for i in range(8):
                if fxm & (0x80 >> i):
                    mk |= 0xF << (28 - 4 * i)
            return f"{G(rt)} = ppu::cr_get(ctx) & 0x{mk:x}u;"
        return f"{G(rt)} = ppu::cr_get(ctx);"
    if m == "mtcrf":
        return f"ppu::cr_put(ctx, 0x{(f.w >> 12) & 0xFF:x}, uint32_t({G(rt)}));"
    if m == "mcrf":
        return f"ctx.cr[{f.crfd}] = ctx.cr[{f.crfs}];"
    if m in ("crand", "cror", "crxor", "crnand", "crnor", "creqv", "crandc", "crorc"):
        a, b = f"ppu::crbit(ctx, {f.ra})", f"ppu::crbit(ctx, {f.rb})"
        expr = {
            "crand": f"{a} && {b}", "cror": f"{a} || {b}", "crxor": f"{a} != {b}", "crnand": f"!({a} && {b})",
            "crnor": f"!({a} || {b})", "creqv": f"{a} == {b}", "crandc": f"{a} && !{b}", "crorc": f"{a} || !{b}",
        }[m]
        return f"ppu::crbit_set(ctx, {f.rt}, {expr});"
    if m == "sync":
        return "std::atomic_thread_fence(std::memory_order_seq_cst);" if not (f.l and (f.w >> 21) & 3 == 1) else "std::atomic_thread_fence(std::memory_order_acq_rel);"
    if m == "eieio":
        return "std::atomic_thread_fence(std::memory_order_release);"
    if m == "isync":
        return "std::atomic_thread_fence(std::memory_order_acquire);"
    if m in ("dcbt", "dcbtst", "dcbst", "dcbf", "icbi"):
        return ";"
    if m == "dcbz":
        return f"ppu::dcbz(base, {ea_x(f)});"
    if m in ("tw", "twi", "td", "tdi"):
        to = f.rt
        if m.endswith("i"):
            b = str(f.simm)
        else:
            b = G(f.rb)
        if m.startswith("tw"):
            a, b2 = f"int32_t({G(ra)})", f"int32_t({b})"
            ua, ub = f"uint32_t({G(ra)})", f"uint32_t({b})"
        else:
            a, b2 = f"int64_t({G(ra)})", f"int64_t({b})"
            ua, ub = G(ra), f"uint64_t({b})"
        if to == 31:
            return "ppu::trap(ctx, base, PC);"
        conds = []
        if to & 16:
            conds.append(f"{a} < {b2}")
        if to & 8:
            conds.append(f"{a} > {b2}")
        if to & 4:
            conds.append(f"{a} == {b2}")
        if to & 2:
            conds.append(f"{ua} < {ub}")
        if to & 1:
            conds.append(f"{ua} > {ub}")
        if not conds:
            return ";"
        return f"if ({' || '.join(conds)}) ppu::trap(ctx, base, PC);"
    if m == "sc":
        return "ctx.cia = PC; ppu::syscall(ctx, base);"
    if m == "mfmsr":
        return f"{G(rt)} = 0x8000000000000000ull;"
    return None


def mask64(mb, me):
    m1 = 0xFFFFFFFFFFFFFFFF >> mb
    m2 = (0xFFFFFFFFFFFFFFFF << (63 - me)) & 0xFFFFFFFFFFFFFFFF
    return (m1 & m2) if mb <= me else (m1 | m2)


def gen_fp(m, f):
    d, a, b, c = f.rt, f.ra, f.rb, f.rc
    if m in ("lfs", "lfd"):
        return f"{F(d)} = ppu::ldf{'32' if m == 'lfs' else '64'}(base, {ea_d(f)});"
    if m in ("lfsu", "lfdu"):
        return f"{{ uint32_t ea = {ea_du(f)}; {F(d)} = ppu::ldf{'32' if m == 'lfsu' else '64'}(base, ea); {G(a)} = {G(a)} + {f.simm}; }}"
    if m in ("lfsx", "lfdx"):
        return f"{F(d)} = ppu::ldf{'32' if m == 'lfsx' else '64'}(base, {ea_x(f)});"
    if m in ("lfsux", "lfdux"):
        return f"{{ uint32_t ea = {ea_xu(f)}; {F(d)} = ppu::ldf{'32' if m == 'lfsux' else '64'}(base, ea); {G(a)} = {G(a)} + {G(b)}; }}"
    if m in ("stfs", "stfd"):
        return f"ppu::stf{'32' if m == 'stfs' else '64'}(base, {ea_d(f)}, {F(d)});"
    if m in ("stfsu", "stfdu"):
        return f"{{ uint32_t ea = {ea_du(f)}; ppu::stf{'32' if m == 'stfsu' else '64'}(base, ea, {F(d)}); {G(a)} = {G(a)} + {f.simm}; }}"
    if m in ("stfsx", "stfdx"):
        return f"ppu::stf{'32' if m == 'stfsx' else '64'}(base, {ea_x(f)}, {F(d)});"
    if m in ("stfsux", "stfdux"):
        return f"{{ uint32_t ea = {ea_xu(f)}; ppu::stf{'32' if m == 'stfsux' else '64'}(base, ea, {F(d)}); {G(a)} = {G(a)} + {G(b)}; }}"
    if m == "stfiwx":
        return f"ppu::st32(base, {ea_x(f)}, uint32_t(ppu::f2u({F(d)})));"
    A, B, C = F(a), F(b), F(c)
    s = {
        "fadd": f"ppu::fnan({A} + {B}, {A}, {B})", "fsub": f"ppu::fnan({A} - {B}, {A}, {B})",
        "fmul": f"ppu::fnan({A} * {C}, {A}, {C})", "fdiv": f"ppu::fnan({A} / {B}, {A}, {B})",
        "fadds": f"ppu::frsp(ppu::fnan({A} + {B}, {A}, {B}))", "fsubs": f"ppu::frsp(ppu::fnan({A} - {B}, {A}, {B}))",
        "fmuls": f"ppu::frsp(ppu::fnan({A} * {C}, {A}, {C}))", "fdivs": f"ppu::frsp(ppu::fnan({A} / {B}, {A}, {B}))",
        "fsqrt": f"ppu::fnan(std::sqrt({B}), {B})", "fsqrts": f"ppu::frsp(ppu::fnan(std::sqrt({B}), {B}))",
        "fmadd": f"ppu::fnan(std::fma({A}, {C}, {B}), {A}, {B}, {C})",
        "fmsub": f"ppu::fnan(std::fma({A}, {C}, -{B}), {A}, {B}, {C})",
        "fnmadd": f"ppu::fneg_r(std::fma({A}, {C}, {B}), {A}, {B}, {C})",
        "fnmsub": f"ppu::fneg_r(std::fma({A}, {C}, -{B}), {A}, {B}, {C})",
        "fmadds": f"ppu::frsp(ppu::fnan(std::fma({A}, {C}, {B}), {A}, {B}, {C}))",
        "fmsubs": f"ppu::frsp(ppu::fnan(std::fma({A}, {C}, -{B}), {A}, {B}, {C}))",
        "fnmadds": f"ppu::frsp(ppu::fneg_r(std::fma({A}, {C}, {B}), {A}, {B}, {C}))",
        "fnmsubs": f"ppu::frsp(ppu::fneg_r(std::fma({A}, {C}, -{B}), {A}, {B}, {C}))",
        "fsel": f"ppu::fsel({A}, {C}, {B})", "fres": f"ppu::frsp(ppu::fres({B}))",
        "frsqrte": f"ppu::frsqrte({B})", "frsp": f"ppu::frsp({B})",
        "fmr": B, "fneg": f"ppu::u2f(ppu::f2u({B}) ^ 0x8000000000000000ull)",
        "fabs": f"ppu::u2f(ppu::f2u({B}) & 0x7FFFFFFFFFFFFFFFull)", "fnabs": f"ppu::u2f(ppu::f2u({B}) | 0x8000000000000000ull)",
        "fctiw": f"ppu::fctiw({B}, false)", "fctiwz": f"ppu::fctiw({B}, true)",
        "fctid": f"ppu::fctid({B}, false)", "fctidz": f"ppu::fctid({B}, true)",
        "fcfid": f"ppu::fcfid({B})",
    }.get(m)
    if s:
        out = f"{F(d)} = {s};"
        if f.rec:
            out += " ctx.cr[1] = {uint8_t(ctx.fpscr >> 31 & 1), uint8_t(ctx.fpscr >> 30 & 1), uint8_t(ctx.fpscr >> 29 & 1), uint8_t(ctx.fpscr >> 28 & 1)};"
        return out
    if m in ("fcmpu", "fcmpo"):
        return f"ppu::fcmp(ctx, {f.crfd}, {F(a)}, {F(b)});"
    if m == "mffs":
        return f"{F(d)} = ppu::u2f(ctx.fpscr);"
    if m == "mtfsf":
        fm = (f.w >> 17) & 0xFF
        mk = 0
        for i in range(8):
            if fm & (0x80 >> i):
                mk |= 0xF << (28 - 4 * i)
        return f"ctx.fpscr = (ctx.fpscr & ~0x{mk:x}u) | (uint32_t(ppu::f2u({F(b)})) & 0x{mk:x}u);"
    if m == "mtfsfi":
        fld = f.crfd
        imm = (f.w >> 12) & 15
        sh = 28 - 4 * fld
        return f"ctx.fpscr = (ctx.fpscr & ~(0xFu << {sh})) | (0x{imm:x}u << {sh});"
    if m in ("mtfsb0", "mtfsb1"):
        bit = 31 - f.rt
        return f"ctx.fpscr = {'ctx.fpscr & ~(1u << ' + str(bit) + ')' if m == 'mtfsb0' else 'ctx.fpscr | (1u << ' + str(bit) + ')'};"
    if m == "mcrfs":
        sh = 28 - 4 * f.crfs
        return f"{{ uint32_t n = ctx.fpscr >> {sh} & 15; ctx.cr[{f.crfd}] = {{uint8_t(n >> 3 & 1), uint8_t(n >> 2 & 1), uint8_t(n >> 1 & 1), uint8_t(n & 1)}}; }}"
    return None


VX_FLOAT = {
    "vaddfp": "_mm_add_ps(ppu::vin(ctx, {a}.vf), ppu::vin(ctx, {b}.vf))",
    "vsubfp": "_mm_sub_ps(ppu::vin(ctx, {a}.vf), ppu::vin(ctx, {b}.vf))",
    "vmaxfp": "ppu::vmax(ppu::vin(ctx, {a}.vf), ppu::vin(ctx, {b}.vf))",
    "vminfp": "ppu::vmin(ppu::vin(ctx, {a}.vf), ppu::vin(ctx, {b}.vf))",
    "vrfin": "_mm_round_ps({b}.vf, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC)",
    "vrfiz": "_mm_round_ps({b}.vf, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC)",
    "vrfip": "_mm_round_ps({b}.vf, _MM_FROUND_TO_POS_INF | _MM_FROUND_NO_EXC)",
    "vrfim": "_mm_round_ps({b}.vf, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC)",
}

VX_INT = {
    "vaddubm": "_mm_add_epi8", "vadduhm": "_mm_add_epi16", "vadduwm": "_mm_add_epi32",
    "vsububm": "_mm_sub_epi8", "vsubuhm": "_mm_sub_epi16", "vsubuwm": "_mm_sub_epi32",
    "vaddubs": "_mm_adds_epu8", "vadduhs": "_mm_adds_epu16", "vaddsbs": "_mm_adds_epi8", "vaddshs": "_mm_adds_epi16",
    "vsububs": "_mm_subs_epu8", "vsubuhs": "_mm_subs_epu16", "vsubsbs": "_mm_subs_epi8", "vsubshs": "_mm_subs_epi16",
    "vmaxub": "_mm_max_epu8", "vmaxuh": "_mm_max_epu16", "vmaxuw": "_mm_max_epu32",
    "vmaxsb": "_mm_max_epi8", "vmaxsh": "_mm_max_epi16", "vmaxsw": "_mm_max_epi32",
    "vminub": "_mm_min_epu8", "vminuh": "_mm_min_epu16", "vminuw": "_mm_min_epu32",
    "vminsb": "_mm_min_epi8", "vminsh": "_mm_min_epi16", "vminsw": "_mm_min_epi32",
    "vavgub": "_mm_avg_epu8", "vavguh": "_mm_avg_epu16",
    "vand": "_mm_and_si128", "vor": "_mm_or_si128", "vxor": "_mm_xor_si128",
    "vcmpequb": "_mm_cmpeq_epi8", "vcmpequh": "_mm_cmpeq_epi16", "vcmpequw": "_mm_cmpeq_epi32",
    "vcmpgtsb": "_mm_cmpgt_epi8", "vcmpgtsh": "_mm_cmpgt_epi16", "vcmpgtsw": "_mm_cmpgt_epi32",
}


def gen_vmx(m, f):
    d, a, b, c = V(f.rt), V(f.ra), V(f.rb), V(f.rc)
    base_m = m[:-1] if m.endswith(".") else m
    dot = m.endswith(".")
    if m in ("lvx", "lvxl", "lvebx", "lvehx", "lvewx"):
        return f"{d} = ppu::ldv(base, {ea_x(f)});"
    if m in ("stvx", "stvxl"):
        return f"ppu::stv(base, {ea_x(f)}, {d});"
    if m == "stvebx":
        return f"{{ uint32_t ea = {ea_x(f)}; ppu::st8(base, ea, {d}.u8[15 - (ea & 15)]); }}"
    if m == "stvehx":
        return f"{{ uint32_t ea = {ea_x(f)} & ~1u; ppu::st16(base, ea, {d}.u16[7 - ((ea & 15) >> 1)]); }}"
    if m == "stvewx":
        return f"{{ uint32_t ea = {ea_x(f)} & ~3u; ppu::st32(base, ea, {d}.u32[3 - ((ea & 15) >> 2)]); }}"
    if m == "lvsl":
        return f"{d} = ppu::lvsl({ea_x(f)});"
    if m == "lvsr":
        return f"{d} = ppu::lvsr({ea_x(f)});"
    if m in ("lvlx", "lvlxl"):
        return f"{d} = ppu::lvlx(base, {ea_x(f)});"
    if m in ("lvrx", "lvrxl"):
        return f"{d} = ppu::lvrx(base, {ea_x(f)});"
    if m in ("stvlx", "stvlxl"):
        return f"ppu::stvlx(base, {ea_x(f)}, {d});"
    if m in ("stvrx", "stvrxl"):
        return f"ppu::stvrx(base, {ea_x(f)}, {d});"
    if base_m in VX_INT:
        s = f"{d}.vi = {VX_INT[base_m]}({a}.vi, {b}.vi);"
        if dot:
            s += f" ppu::vcr6(ctx, {d}.vi);"
        return s
    if m in ("vcmpgtub", "vcmpgtuh", "vcmpgtuw", "vcmpgtub.", "vcmpgtuh.", "vcmpgtuw."):
        bits = {"b": ("8", "0x80"), "h": ("16", "0x8000"), "w": ("32", "0x80000000")}[base_m[-1]]
        sx = f"_mm_set1_epi{bits[0]}(int{bits[0]}_t({bits[1]}))" if bits[0] != "32" else "_mm_set1_epi32(int32_t(0x80000000u))"
        s = f"{d}.vi = _mm_cmpgt_epi{bits[0]}(_mm_xor_si128({a}.vi, {sx}), _mm_xor_si128({b}.vi, {sx}));"
        if dot:
            s += f" ppu::vcr6(ctx, {d}.vi);"
        return s
    if base_m in ("vcmpeqfp", "vcmpgefp", "vcmpgtfp"):
        op = {"vcmpeqfp": "_mm_cmpeq_ps", "vcmpgefp": "_mm_cmpge_ps", "vcmpgtfp": "_mm_cmpgt_ps"}[base_m]
        s = f"{d}.vf = {op}({a}.vf, {b}.vf);"
        if dot:
            s += f" ppu::vcr6(ctx, {d}.vi);"
        return s
    if base_m == "vcmpbfp":
        s = (f"{{ __m128 le = _mm_cmple_ps({a}.vf, {b}.vf); __m128 ge = _mm_cmpge_ps({a}.vf, _mm_sub_ps(_mm_setzero_ps(), {b}.vf)); "
             f"{d}.vi = _mm_or_si128(_mm_andnot_si128(_mm_castps_si128(le), _mm_set1_epi32(int32_t(0x80000000u))), _mm_andnot_si128(_mm_castps_si128(ge), _mm_set1_epi32(0x40000000))); }}")
        if dot:
            s += f" ctx.cr[6] = {{0, 0, uint8_t(_mm_testz_si128({d}.vi, {d}.vi)), 0}};"
        return s
    if m in VX_FLOAT:
        srcs = f"{a}.vf, {b}.vf, {b}.vf" if m in ("vaddfp", "vsubfp", "vmaxfp", "vminfp") else f"{b}.vf, {b}.vf, {b}.vf"
        return f"{d}.vf = ppu::vfix(ctx, {VX_FLOAT[m].format(a=a, b=b)}, {srcs});"
    if m == "vmaddfp":
        return f"{d}.vf = ppu::vfix0(ctx, _mm_fmadd_ps(ppu::vin(ctx, {a}.vf), ppu::vin(ctx, {c}.vf), ppu::vin(ctx, {b}.vf)));"
    if m == "vnmsubfp":
        return f"{d}.vf = ppu::vfix0(ctx, _mm_xor_ps(_mm_fmsub_ps(ppu::vin(ctx, {a}.vf), ppu::vin(ctx, {c}.vf), ppu::vin(ctx, {b}.vf)), _mm_castsi128_ps(_mm_set1_epi32(int32_t(0x80000000u)))));"
    if m in ("vexptefp", "vlogefp"):
        return f"{{ ppu::v128 t = {b}; for (int i = 0; i < 4; i++) t.u32[i] = ppu::{m}(t.u32[i], ctx.vscr_nj); {d} = t; }}"
    if m in ("vrefp", "vrsqrtefp"):
        fn = {"vrefp": "ppu::vrefp", "vrsqrtefp": "ppu::vrsqrtefp"}[m]
        return f"{{ ppu::v128 t; t.vf = ppu::vin(ctx, {b}.vf); for (int i = 0; i < 4; i++) t.f32[i] = {fn}(t.f32[i]); {d}.vf = ppu::vfix(ctx, t.vf, {b}.vf, {b}.vf, {b}.vf); }}"
    if m == "vandc":
        return f"{d}.vi = _mm_andnot_si128({b}.vi, {a}.vi);"
    if m == "vnor":
        return f"{d}.vi = _mm_xor_si128(_mm_or_si128({a}.vi, {b}.vi), _mm_set1_epi32(-1));"
    if m == "vsel":
        return f"{d}.vi = _mm_or_si128(_mm_andnot_si128({c}.vi, {a}.vi), _mm_and_si128({c}.vi, {b}.vi));"
    if m == "vperm":
        return f"{d}.vi = ppu::vperm({a}.vi, {b}.vi, {c}.vi);"
    if m == "vsldoi":
        sh = (f.w >> 6) & 15
        return f"{d}.vi = _mm_alignr_epi8({a}.vi, {b}.vi, {16 - sh});" if sh else f"{d} = {a};"
    if m == "vspltw":
        i = 3 - (f.ra & 3)
        return f"{d}.vi = _mm_shuffle_epi32({b}.vi, {i * 0x55});"
    if m == "vsplth":
        i = 7 - (f.ra & 7)
        return f"{d}.vi = _mm_set1_epi16(int16_t({b}.u16[{i}]));"
    if m == "vspltb":
        i = 15 - (f.ra & 15)
        return f"{d}.vi = _mm_set1_epi8(int8_t({b}.u8[{i}]));"
    if m in ("vspltisb", "vspltish", "vspltisw"):
        v = sext(f.ra, 5)
        w = {"vspltisb": "8", "vspltish": "16", "vspltisw": "32"}[m]
        return f"{d}.vi = _mm_set1_epi{w}({v});"
    if m in ("vmrghb", "vmrghh", "vmrghw", "vmrglb", "vmrglh", "vmrglw"):
        hl = "hi" if m[4] == "h" else "lo"
        w = {"b": "8", "h": "16", "w": "32"}[m[5]]
        return f"{d}.vi = _mm_unpack{hl}_epi{w}({b}.vi, {a}.vi);"
    if m == "vpkswss":
        return f"{d}.vi = _mm_packs_epi32({b}.vi, {a}.vi);"
    if m == "vpkshss":
        return f"{d}.vi = _mm_packs_epi16({b}.vi, {a}.vi);"
    if m == "vpkswus":
        return f"{d}.vi = _mm_packus_epi32({b}.vi, {a}.vi);"
    if m == "vpkshus":
        return f"{d}.vi = _mm_packus_epi16({b}.vi, {a}.vi);"
    if m == "vpkuwus":
        return f"{d}.vi = _mm_packus_epi32(_mm_min_epu32({b}.vi, _mm_set1_epi32(0xFFFF)), _mm_min_epu32({a}.vi, _mm_set1_epi32(0xFFFF)));"
    if m == "vpkuhus":
        return f"{d}.vi = _mm_packus_epi16(_mm_min_epu16({b}.vi, _mm_set1_epi16(0xFF)), _mm_min_epu16({a}.vi, _mm_set1_epi16(0xFF)));"
    if m == "vpkuwum":
        return f"{d}.vi = _mm_packus_epi32(_mm_and_si128({b}.vi, _mm_set1_epi32(0xFFFF)), _mm_and_si128({a}.vi, _mm_set1_epi32(0xFFFF)));"
    if m == "vpkuhum":
        return f"{d}.vi = _mm_packus_epi16(_mm_and_si128({b}.vi, _mm_set1_epi16(0xFF)), _mm_and_si128({a}.vi, _mm_set1_epi16(0xFF)));"
    if m in ("vupkhsb", "vupkhsh", "vupklsb", "vupklsh"):
        src = f"_mm_srli_si128({b}.vi, 8)" if m[4] == "h" else f"{b}.vi"
        fn = "_mm_cvtepi8_epi16" if m[-1] == "b" else "_mm_cvtepi16_epi32"
        return f"{d}.vi = {fn}({src});"
    if m in ("vslb", "vslh", "vslw", "vsrb", "vsrh", "vsrw", "vsrab", "vsrah", "vsraw", "vrlb", "vrlh", "vrlw"):
        n = {"b": 8, "h": 16, "w": 32}[m[-1]]
        cnt = {"b": 16, "h": 8, "w": 4}[m[-1]]
        fld = {"b": ("u8", "s8", "uint8_t"), "h": ("u16", "s16", "uint16_t"), "w": ("u32", "s32", "uint32_t")}[m[-1]]
        u, s, t = fld
        op = m[1:-1]
        if op == "sl":
            e = f"{t}(x.{u}[i] << (y.{u}[i] & {n - 1}))"
        elif op == "sr":
            e = f"{t}(x.{u}[i] >> (y.{u}[i] & {n - 1}))"
        elif op == "sra":
            e = f"{t}(x.{s}[i] >> (y.{u}[i] & {n - 1}))"
        else:
            e = f"{t}((x.{u}[i] << (y.{u}[i] & {n - 1})) | (x.{u}[i] >> (({n} - (y.{u}[i] & {n - 1})) & {n - 1})))"
        return f"{{ ppu::v128 x = {a}, y = {b}, r; for (int i = 0; i < {cnt}; i++) r.{u}[i] = {e}; {d} = r; }}"
    if m in ("vsl", "vsr"):
        op = "<<" if m == "vsl" else ">>"
        inv = ">>" if m == "vsl" else "<<"
        return (f"{{ ppu::v128 x = {a}; uint32_t s = {b}.u8[0] & 7; if (s) {{ ppu::v128 r; "
                f"r.u64[{'1' if m == 'vsl' else '0'}] = (x.u64[{'1' if m == 'vsl' else '0'}] {op} s) | (x.u64[{'0' if m == 'vsl' else '1'}] {inv} (64 - s)); "
                f"r.u64[{'0' if m == 'vsl' else '1'}] = x.u64[{'0' if m == 'vsl' else '1'}] {op} s; {d} = r; }} else {d} = x; }}")
    if m in ("vslo", "vsro"):
        shift = "_mm_bslli_si128" if m == "vslo" else "_mm_bsrli_si128"
        return f"{{ ppu::v128 x = {a}; uint32_t s = ({b}.u8[0] >> 3) & 15; ppu::v128 r{{}}; for (int i = 0; i < 16; i++) {{ int j = {'i - int(s)' if m == 'vslo' else 'i + int(s)'}; r.u8[i] = (j >= 0 && j < 16) ? x.u8[j] : 0; }} {d} = r; }}"
    if m in ("vcfsx", "vcfux"):
        sc = f.ra
        conv = "_mm_cvtepi32_ps({b}.vi)" if m == "vcfsx" else "_mm_add_ps(_mm_cvtepi32_ps(_mm_srli_epi32({b}.vi, 1)), _mm_cvtepi32_ps(_mm_srli_epi32({b}.vi, 1)))"
        if m == "vcfux":
            conv = "_mm_add_ps(_mm_mul_ps(_mm_cvtepi32_ps(_mm_srli_epi32({b}.vi, 16)), _mm_set1_ps(65536.0f)), _mm_cvtepi32_ps(_mm_and_si128({b}.vi, _mm_set1_epi32(0xFFFF))))"
        return f"{d}.vf = _mm_mul_ps({conv.format(b=b)}, _mm_set1_ps({1.0 / (1 << sc)!r}f));"
    if m in ("vctsxs", "vctuxs"):
        sc = f.ra
        if m == "vctsxs":
            return (f"{{ ppu::v128 t = {b}; for (int i = 0; i < 4; i++) {{ double v = double(t.f32[i]) * {float(1 << sc)!r}; "
                    f"t.s32[i] = std::isnan(t.f32[i]) ? 0 : v >= 2147483647.0 ? INT32_MAX : v <= -2147483648.0 ? INT32_MIN : int32_t(v); }} {d} = t; }}")
        return (f"{{ ppu::v128 t = {b}; for (int i = 0; i < 4; i++) {{ double v = double(t.f32[i]) * {float(1 << sc)!r}; "
                f"t.u32[i] = std::isnan(t.f32[i]) ? 0 : v >= 4294967295.0 ? UINT32_MAX : v <= 0.0 ? 0 : uint32_t(v); }} {d} = t; }}")
    if m == "mfvscr":
        return f"{d}.vi = _mm_set_epi32(0, 0, 0, int32_t(ctx.vscr_nj << 16 | ctx.vscr_sat));"
    if m == "mtvscr":
        return f"ctx.vscr_nj = {b}.u32[0] >> 16 & 1; ctx.vscr_sat = {b}.u32[0] & 1;"
    if m in ("vmuloub", "vmulouh", "vmulosb", "vmulosh", "vmuleub", "vmuleuh", "vmulesb", "vmulesh"):
        signed = m[5] == "s"
        half = m[-1] == "h"
        src = ("s16" if signed else "u16") if half else ("s8" if signed else "u8")
        dst = ("s32" if signed else "u32") if half else ("s16" if signed else "u16")
        cnt = 4 if half else 8
        odd = m[4] == "o"
        idx = "2 * i" if odd else "2 * i + 1"
        return f"{{ ppu::v128 x = {a}, y = {b}, r; for (int i = 0; i < {cnt}; i++) r.{dst}[i] = x.{src}[{idx}] * y.{src}[{idx}]; {d} = r; }}"
    if m in ("vmhaddshs", "vmhraddshs", "vmladduhm"):
        if m == "vmladduhm":
            return f"{d}.vi = _mm_add_epi16(_mm_mullo_epi16({a}.vi, {b}.vi), {c}.vi);"
        rnd = " + 0x4000" if m == "vmhraddshs" else ""
        return f"{{ ppu::v128 x = {a}, y = {b}, z = {c}, r; for (int i = 0; i < 8; i++) {{ int32_t v = ((int32_t(x.s16[i]) * y.s16[i]{rnd}) >> 15) + z.s16[i]; r.s16[i] = int16_t(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }} {d} = r; }}"
    if m in ("vmsumubm", "vmsummbm", "vmsumuhm", "vmsumuhs", "vmsumshm", "vmsumshs"):
        half = m[6] == "h"
        sa = {"vmsumubm": "u8", "vmsummbm": "s8", "vmsumuhm": "u16", "vmsumuhs": "u16", "vmsumshm": "s16", "vmsumshs": "s16"}[m]
        sb = {"vmsumubm": "u8", "vmsummbm": "u8", "vmsumuhm": "u16", "vmsumuhs": "u16", "vmsumshm": "s16", "vmsumshs": "s16"}[m]
        per = 2 if half else 4
        sat = m.endswith("s")
        acc = "int64_t" if sat else "uint32_t"
        tail = ""
        if sat and m == "vmsumshs":
            tail = "r.s32[i] = int32_t(s > INT32_MAX ? INT32_MAX : s < INT32_MIN ? INT32_MIN : s);"
        elif sat:
            tail = "r.u32[i] = uint32_t(s > UINT32_MAX ? UINT32_MAX : s);"
        else:
            tail = "r.u32[i] = uint32_t(s);"
        init = "int64_t(z.s32[i])" if m == "vmsumshs" else ("int64_t(z.u32[i])" if sat else "z.u32[i]")
        return (f"{{ ppu::v128 x = {a}, y = {b}, z = {c}, r; for (int i = 0; i < 4; i++) {{ {acc} s = {init}; "
                f"for (int j = 0; j < {per}; j++) s += {acc}(x.{sa}[i * {per} + j]) * {acc}(y.{sb}[i * {per} + j]); {tail} }} {d} = r; }}")
    if m in ("vsum4ubs", "vsum4sbs", "vsum4shs", "vsum2sws", "vsumsws"):
        if m == "vsumsws":
            return f"{{ ppu::v128 x = {a}, y = {b}, r{{}}; int64_t s = y.s32[0]; for (int i = 0; i < 4; i++) s += x.s32[i]; r.s32[0] = int32_t(s > INT32_MAX ? INT32_MAX : s < INT32_MIN ? INT32_MIN : s); {d} = r; }}"
        if m == "vsum2sws":
            return f"{{ ppu::v128 x = {a}, y = {b}, r{{}}; for (int k = 0; k < 2; k++) {{ int64_t s = int64_t(y.s32[2 * k]) + x.s32[2 * k] + x.s32[2 * k + 1]; r.s32[2 * k] = int32_t(s > INT32_MAX ? INT32_MAX : s < INT32_MIN ? INT32_MIN : s); }} {d} = r; }}"
        src, per = {"vsum4ubs": ("u8", 4), "vsum4sbs": ("s8", 4), "vsum4shs": ("s16", 2)}[m]
        if m == "vsum4ubs":
            return f"{{ ppu::v128 x = {a}, y = {b}, r; for (int i = 0; i < 4; i++) {{ uint64_t s = y.u32[i]; for (int j = 0; j < 4; j++) s += x.u8[i * 4 + j]; r.u32[i] = uint32_t(s > UINT32_MAX ? UINT32_MAX : s); }} {d} = r; }}"
        return f"{{ ppu::v128 x = {a}, y = {b}, r; for (int i = 0; i < 4; i++) {{ int64_t s = y.s32[i]; for (int j = 0; j < {per}; j++) s += x.{src}[i * {per} + j]; r.s32[i] = int32_t(s > INT32_MAX ? INT32_MAX : s < INT32_MIN ? INT32_MIN : s); }} {d} = r; }}"
    if m in ("vaddcuw", "vsubcuw"):
        if m == "vaddcuw":
            return f"{{ ppu::v128 x = {a}, y = {b}, r; for (int i = 0; i < 4; i++) r.u32[i] = uint32_t(uint64_t(x.u32[i]) + y.u32[i] > UINT32_MAX); {d} = r; }}"
        return f"{{ ppu::v128 x = {a}, y = {b}, r; for (int i = 0; i < 4; i++) r.u32[i] = uint32_t(x.u32[i] >= y.u32[i]); {d} = r; }}"
    if m in ("vadduws", "vaddsws", "vsubuws", "vsubsws", "vavguw", "vavgsb", "vavgsh", "vavgsw"):
        e = {
            "vadduws": ("u32", "uint64_t s = uint64_t(x.u32[i]) + y.u32[i]; r.u32[i] = uint32_t(s > UINT32_MAX ? UINT32_MAX : s);"),
            "vaddsws": ("s32", "int64_t s = int64_t(x.s32[i]) + y.s32[i]; r.s32[i] = int32_t(s > INT32_MAX ? INT32_MAX : s < INT32_MIN ? INT32_MIN : s);"),
            "vsubuws": ("u32", "r.u32[i] = x.u32[i] > y.u32[i] ? x.u32[i] - y.u32[i] : 0;"),
            "vsubsws": ("s32", "int64_t s = int64_t(x.s32[i]) - y.s32[i]; r.s32[i] = int32_t(s > INT32_MAX ? INT32_MAX : s < INT32_MIN ? INT32_MIN : s);"),
            "vavguw": ("u32", "r.u32[i] = uint32_t((uint64_t(x.u32[i]) + y.u32[i] + 1) >> 1);"),
            "vavgsw": ("s32", "r.s32[i] = int32_t((int64_t(x.s32[i]) + y.s32[i] + 1) >> 1);"),
            "vavgsh": ("s16", "r.s16[i] = int16_t((int32_t(x.s16[i]) + y.s16[i] + 1) >> 1);"),
            "vavgsb": ("s8", "r.s8[i] = int8_t((int32_t(x.s8[i]) + y.s8[i] + 1) >> 1);"),
        }[m]
        cnt = {"32": 4, "16": 8, "8": 16}[e[0][1:]]
        return f"{{ ppu::v128 x = {a}, y = {b}, r; for (int i = 0; i < {cnt}; i++) {{ {e[1]} }} {d} = r; }}"
    if m in ("vpkpx", "vupkhpx", "vupklpx"):
        return None
    return None


def gen_insn(m, w):
    f = Fields(w)
    for g in (gen_int, gen_fp, gen_vmx):
        s = g(m, f)
        if s is not None:
            return s
    return None


class Emitter:
    def __init__(self, img, funcs, entries, import_base=0):
        self.img = img
        self.funcs = funcs
        self.entries = entries
        self.import_base = import_base
        self.unimpl = {}

    def fname(self, addr):
        return f"f_{addr:08x}"

    def call(self, target, ret):
        if target in self.img.stubs:
            return f"ctx.lr = 0x{ret:x}; ppu::call_import(ctx, base, {self.import_base + self.img.stubs[target]});"
        if target in self.entries:
            return f"ctx.lr = 0x{ret:x}; {self.fname(target)}(ctx, base);"
        return f"ctx.lr = 0x{ret:x}; ppu::call_indirect(ctx, base, 0x{target:x});"

    def jump_out(self, target, f):
        if target in self.img.stubs:
            return f"ppu::call_import(ctx, base, {self.import_base + self.img.stubs[target]}); return;"
        if target in self.entries:
            return f"{self.fname(target)}(ctx, base); return;"
        return f"ppu::call_indirect(ctx, base, 0x{target:x}); return;"

    def goto(self, target, f):
        if target in f.insns and not (target in self.entries and target != f.entry):
            return f"goto L_{target:x};"
        return self.jump_out(target, f)

    def function(self, f):
        insns = sorted(f.insns)
        labels = {f.entry}
        for pc in insns:
            w = self.img.word(pc)
            op = w >> 26
            if op in (16, 18) and not w & 1:
                t = branch_target(pc, w)
                if t in f.insns:
                    labels.add(t)
        for jt in f.jump_tables.values():
            labels.update(t for t in jt["targets"] if t in f.insns)
        name = self.fname(f.entry)
        out = [f"static void {name}_body(ppu::Context& ctx, uint8_t* base) {{"]
        if insns and insns[0] != f.entry:
            out.append(f"    goto L_{f.entry:x};")
        for n, pc in enumerate(insns):
            w = self.img.word(pc)
            m = mnemonic(w)
            if pc in labels:
                out.append(f"L_{pc:x}:")
            body = self.control(pc, w, m, f)
            if body is None:
                body = gen_insn(m, w)
                if body is None:
                    self.unimpl[m] = self.unimpl.get(m, 0) + 1
                    body = f"ppu::unimplemented(ctx, base, 0x{pc:x}, 0x{w:08x}u);"
                body = body.replace("PC", f"0x{pc:x}")
                falls = True
            else:
                falls = self.falls_through(w, m)
            out.append(f"    {body}")
            nxt = pc + 4
            if falls and (n + 1 >= len(insns) or insns[n + 1] != nxt):
                if nxt in f.insns:
                    out.append(f"    goto L_{nxt:x};")
                else:
                    out.append(f"    {self.jump_out(nxt, f)}")
        out.append("}")
        out.append(f"void {name}(ppu::Context& ctx, uint8_t* base) {{")
        out.append(f"    if (ppu::hook_flags[0x{f.entry >> 2:x}]) [[unlikely]] {{")
        out.append(f"        ppu::hook_call(ctx, base, 0x{f.entry:x}, {name}_body);")
        out.append("        return;")
        out.append("    }")
        out.append(f"    {name}_body(ctx, base);")
        out.append("}")
        return "\n".join(out)

    def falls_through(self, w, m):
        op = w >> 26
        if op == 18:
            return bool(w & 1) or False
        if op == 16:
            return bool(w & 1) or not bo_always(w)
        if m in ("bclr", "bcctr"):
            return bool(w & 1) or not bo_always(w)
        if w == 0x7FE00008:
            return False
        return True

    def control(self, pc, w, m, f):
        op = w >> 26
        if op == 18:
            t = branch_target(pc, w)
            if w & 1:
                return self.call(t, pc + 4)
            return self.goto(t, f)
        if op == 16:
            t = branch_target(pc, w)
            bo, bi = (w >> 21) & 31, (w >> 16) & 31
            act = self.call(t, pc + 4) if w & 1 else self.goto(t, f)
            if bo_always(w) and bo & 4:
                return act
            return f"if (ppu::bc_cond(ctx, {bo}, {bi})) {{ {act} }}"
        if m in ("bclr", "bcctr"):
            bo, bi = (w >> 21) & 31, (w >> 16) & 31
            cond = None if (bo_always(w) and bo & 4) else f"ppu::bc_cond(ctx, {bo}, {bi})"
            if m == "bclr":
                act = f"{{ uint64_t t = ctx.lr; ctx.lr = 0x{pc + 4:x}; ppu::call_indirect(ctx, base, uint32_t(t)); }}" if w & 1 else "return;"
            elif w & 1:
                act = f"ctx.lr = 0x{pc + 4:x}; ppu::call_indirect(ctx, base, uint32_t(ctx.ctr));"
            elif pc in f.jump_tables:
                jt = f.jump_tables[pc]
                cases = " ".join(f"case 0x{t:x}: goto L_{t:x};" for t in sorted(set(jt["targets"])))
                act = f"switch (uint32_t(ctx.ctr)) {{ {cases} default: ppu::call_indirect(ctx, base, uint32_t(ctx.ctr)); return; }}"
            else:
                act = "ppu::call_indirect(ctx, base, uint32_t(ctx.ctr)); return;"
            return f"if ({cond}) {{ {act} }}" if cond else act
        return None


HEADER = '#include "ppu/ops.h"\n#include "funcs.h"\n\n'


def lift(img):
    funcs, entries = discover(img)
    extra = set()
    while True:
        isl, _ = islands(img, funcs)
        found = {a for a, b in isl if looks_like_function(img, a, b)} - extra
        if not found:
            break
        extra |= found
        funcs, entries = discover(img, extra)
    return funcs, entries


NL = "\n"


def main(manifest_path, nid_db, out_dir, chunk_bytes="0x19000"):
    import json
    nids = json.load(open(nid_db))
    root = os.path.dirname(os.path.abspath(manifest_path))
    manifest = json.load(open(manifest_path))
    os.makedirs(out_dir, exist_ok=True)
    for old in os.listdir(out_dir):
        if old.endswith(".cpp") or old.endswith(".h"):
            os.remove(os.path.join(out_dir, old))
    chunk = int(chunk_bytes, 16)
    all_funcs = []
    images = []
    imports = []
    exports = []
    unimpl = {}
    for idx, spec in enumerate(manifest):
        img = Image(os.path.join(root, spec["elf"]), nids)
        funcs, entries = lift(img)
        em = Emitter(img, funcs, entries, len(imports))
        for imp in img.imports:
            imports.append(dict(imp, image=idx))
        for exp in img.exports:
            exports.append(dict(exp, image=idx))
        order = sorted(funcs)
        files = []
        cur, buf = None, []
        for e in order:
            bucket = (e - img.text_lo) // chunk
            if bucket != cur:
                if buf:
                    files.append((cur, buf))
                cur, buf = bucket, []
            buf.append(em.function(funcs[e]))
        if buf:
            files.append((cur, buf))
        for bucket, bodies in files:
            with open(os.path.join(out_dir, f"{spec['name']}_{bucket:03d}.cpp"), "w") as fo:
                fo.write(HEADER)
                fo.write((NL + NL).join(bodies))
                fo.write(NL)
        all_funcs.extend((e, em.fname(e)) for e in order)
        images.append({"name": spec["name"], "file": spec["file"], "prx": bool(img.exports),
                       "text_lo": img.text_lo, "text_hi": img.text_hi, "toc": img.toc, "entry": img.elf.entry})
        for k, v in em.unimpl.items():
            unimpl[k] = unimpl.get(k, 0) + v
        print(f"{spec['name']:14} functions {len(order):>6} files {len(files):>3} imports {len(img.imports):>4} exports {len(img.exports):>4}")
    all_funcs.sort()
    lines = ["#pragma once", '#include "ppu/context.h"', ""]
    lines += [f"void {name}(ppu::Context& ctx, uint8_t* base);" for _, name in all_funcs]
    open(os.path.join(out_dir, "funcs.h"), "w").write(NL.join(lines) + NL)
    t = ['#include "ppu/context.h"', '#include "funcs.h"', '#include "runtime/image.h"', "", "namespace image {", ""]
    t.append("const ImageEntry images[] = {")
    for im in images:
        t.append(f'    {{"{im["name"]}", "{im["file"]}", {str(im["prx"]).lower()}, 0x{im["text_lo"]:x}, 0x{im["text_hi"]:x}, 0x{im["toc"]:x}, 0x{im["entry"]:x}}},')
    t += ["};", f"const uint32_t image_count = {len(images)};", ""]
    t.append("const FuncEntry functions[] = {")
    t += [f"    {{0x{addr:x}, {name}}}," for addr, name in all_funcs]
    t += ["};", f"const uint32_t function_count = {len(all_funcs)};", ""]
    t.append("const ImportEntry imports[] = {")
    t += [f'    {{"{imp["module"]}", 0x{imp["nid"]}u, "{imp["name"] or ""}", {imp["image"]}}},' for imp in imports]
    t += ["};", f"const uint32_t import_count = {len(imports)};", ""]
    t.append("const ExportEntry exports[] = {")
    t += [f'    {{"{exp["module"]}", 0x{exp["nid"]}u, "{exp["name"] or ""}", {exp["image"]}, 0x{exp["opd"]:x}}},' for exp in exports]
    t += ["};", f"const uint32_t export_count = {len(exports)};", "", "}"]
    open(os.path.join(out_dir, "table.cpp"), "w").write(NL.join(t) + NL)
    print(f"total functions {len(all_funcs)} imports {len(imports)} exports {len(exports)}")
    print("unimplemented:", sorted(unimpl.items(), key=lambda kv: -kv[1]))


if __name__ == "__main__":
    main(*sys.argv[1:])
