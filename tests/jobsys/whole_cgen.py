import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "renjob"))
import rj_cgen as G

MODULES = [
    (0x730800, "TexCreator"),
    (0x6f7200, "TexStreaming"),
    (0x743a00, "ColorCorrection"),
    (0x766280, "ReadPackets"),
    (0x77a580, "WritePackets"),
    (0x728400, "Cloth"),
    (0x7a5c80, "StereoDepth"),
    (0x71c500, "WaterProc"),
]

PM_CALLS = {0x017b8: "renjob::wh::pm_gettag(R, ls, ctl);", 0x017c0: "renjob::wh::pm_releasetag(R, ls, ctl);", 0x017e0: ""}
CALL_RE = re.compile(r"(r0 = pref_only\(0x[0-9a-f]+\); \{\s*)f_([0-9a-f]{5})\(R, ls, ctl\); if \(ctl\.out\) return;")


def header(ea):
    return struct.unpack_from(">12I", G.data, ea - 0x10000)


def fresh():
    G.TU.clear()
    G.TD.clear()
    G.SPEC.clear()
    G.NOCALL.clear()
    G.REC_EDGES.clear()


def continuations(f, funcs):
    out = set()
    for pc, d in f.insns.items():
        if d["op"] in ("ila", "il") and d["rt"] == 0:
            t = (d["imm"] if d["op"] == "ila" else d["imm"] & 0xFFFF) & 0x3FFFC
            if 0x4000 <= t < G.CODE_END and t not in funcs and t != pc + 4:
                out.add(t)
    return sorted(out)


def gen_module(ea, name, gen_dir):
    h = header(ea)
    entry = 0x4000 + h[4]
    size = min(h[5], 0x3a000)
    fresh()
    G.configure(IMG_EA=ea, IMG_SIZE=size, CODE_END=0x4000 + size, ENTRY_ROOTS=(entry,), INLINE_EXCEPT=(), ICALLS={}, RECURSION_LIMIT=0)
    G.WHOLE = True
    G.EXITS = False
    G.KERNELS = {}
    G.DOPS3 = ("dfma", "dfms", "dfnms", "dfnma")
    G.RR3.update({"dfa", "dfs", "dfm", "dfceq", "dfcgt", "dfcmeq", "dfcmgt"})
    G.RR2.update({"fesd", "frds"})
    G.HINTS = False
    G.PC_COUNTS = {}
    known = {entry}
    root = G.discover(entry, known, frozenset())
    conts = [c for c in continuations(root, set()) if c < entry + 0x400 and c > entry]
    multi = {entry: tuple([entry] + conts)} if conts else {}
    funcs = {}
    changed = True
    while changed:
        changed = False
        for e in sorted(known):
            if e in funcs:
                continue
            try:
                funcs[e] = G.discover(e, known, frozenset(), multi.get(e))
            except Exception as ex:
                print("%s: discover %05x failed: %s" % (name, e, ex), file=sys.stderr)
                return None
            for c in funcs[e].calls | funcs[e].tails:
                if c not in known and 0x4000 <= c < G.CODE_END:
                    known.add(c)
                    changed = True
    for e, f in funcs.items():
        f.calls = {c for c in f.calls if c in funcs}
        f.tails = {c for c in f.tails if c in funcs}
    ns = "renjob::jw%06x" % ea
    base = "w_%06x_gen.h" % ea
    out = ['#include "%s"' % base, '#include "jwhole_rt.h"', '#include "jw_dops.h"', "", "namespace %s {" % ns, "", "using namespace renjob::xo;", "", "#define FAIL(pc) do { ctl.fail = true; ctl.out = true; ctl.fail_pc = pc; return; } while (0)", ""]
    inline_max = 120
    for e in sorted(funcs):
        if e != entry and len(funcs[e].insns) <= inline_max and not (funcs[e].calls & set(funcs)):
            G.SPEC[e] = "static inline __attribute__((always_inline)) "
    out.append("void icall(V* R, uint8_t* ls, Ctl& ctl, uint32_t t, uint32_t pc);")
    for e in sorted(funcs):
        out.append("%svoid f_%05x(V* R, uint8_t* ls, Ctl& ctl%s);" % (G.SPEC.get(e, ""), e, ", uint32_t entry" if e in multi else ""))
    out.append("")
    G.closure(funcs)
    body = []
    try:
        for e in sorted(funcs):
            G.emit_func(funcs[e], body)
    except Exception as ex:
        print("%s: emit failed: %s" % (name, ex), file=sys.stderr)
        return None
    out += body
    cases = " ".join("case 0x%05x: f_%05x(R, ls, ctl%s); return;" % (e, e, ", 0x%05x" % e if e in multi else "") for e in sorted(funcs) if e not in G.SPEC)
    cases += " " + " ".join("case 0x%05x: %s return;" % (t, c) for t, c in sorted(PM_CALLS.items()))
    out.append("void icall(V* R, uint8_t* ls, Ctl& ctl, uint32_t t, uint32_t pc) { if (jobsys::jw::hle_call(R, ls, ctl, t & 0x3fffc)) return; switch (t & 0x3fffc) { %s default: ctl.icall_miss = t; FAIL(pc); } }" % cases)
    out = [re.sub(r"(?<![A-Za-z0-9_])st\(ls,", "stp(ls,", l) for l in out]
    out = [re.sub(r"(?<![A-Za-z0-9_])st32\(ls,", "st32p(ls,", l) for l in out]
    out = [CALL_RE.sub(lambda m: m.group(1) + "if (!jobsys::jw::hle_call(R, ls, ctl, 0x%s)) f_%s(R, ls, ctl); if (ctl.out) return;" % (m.group(2), m.group(2)), l) for l in out]
    out = [G.abi(l) for l in out]
    run = ["", "jobsys::WholeResult run(uint8_t* ls, spu::Context& c, renjob::ChanHost& h) {", "    jobsys::jw::Frame fr(ls, c, h);", "    V* R = fr.R;", "    Ctl& ctl = fr.ctl;"]
    run.append("    f_%05x(R, ls, ctl%s);" % (entry, ", 0x%05x" % entry if multi else ""))
    run.append("    for (int k = 0; k < 8 && !ctl.fail; k++) {")
    run.append("        uint32_t pc = pref(R[0]) & 0x3fffc;")
    run.append("        if (pc == 0x3368) break;")
    for cpc in conts:
        run.append("        if (pc == 0x%05x) { ctl.out = false; f_%05x(R, ls, ctl, 0x%05x); continue; }" % (cpc, entry, cpc))
    run.append("        ctl.fail = true;")
    run.append("        ctl.fail_pc = pc;")
    run.append("    }")
    run.append("    return fr.finish();")
    run.append("}")
    out += run
    out += ["", "#undef FAIL", "", "}"]
    open(os.path.join(gen_dir, "w_%06x_gen.cpp" % ea), "w", newline="\n").write("\n".join(out) + "\n")
    hh = ["#pragma once", "", '#include "renjob_ops.h"', '#include "runtime/jobsys.h"', "", "namespace %s {" % ns, "", "using namespace renjob::xo;", ""]
    hh.append("inline constexpr uint32_t kEntry = 0x%05x;" % entry)
    hh.append("inline constexpr uint32_t kSig[8] = {%s};" % ", ".join("0x%08xu" % G.word(entry + 4 * i) for i in range(8)))
    hh.append("jobsys::WholeResult run(uint8_t* ls, spu::Context& c, renjob::ChanHost& h);")
    hh += ["", "}"]
    open(os.path.join(gen_dir, base), "w", newline="\n").write("\n".join(hh) + "\n")
    n = sum(len(f.insns) for f in funcs.values())
    print("%s 0x%06x: entry 0x%05x conts %s, %d functions, %d insns, %d with fail" % (name, ea, entry, ["%05x" % c for c in conts], len(funcs), n, sum(1 for f in funcs.values() if f.has_fail)))
    return entry


def main():
    gen_dir = sys.argv[1]
    only = set(int(x, 16) for x in sys.argv[2].split(",")) if len(sys.argv) > 2 and sys.argv[2] else None
    os.makedirs(gen_dir, exist_ok=True)
    done = []
    for ea, name in MODULES:
        if only and ea not in only:
            continue
        e = gen_module(ea, name, gen_dir)
        if e:
            done.append((ea, name, e))
    lines = ["#pragma once", ""]
    for ea, name, e in done:
        lines.append('#include "w_%06x_gen.h"' % ea)
    lines.append("")
    lines.append("#define JW_MODULES(X) " + " ".join('X(0x%06x, "%s", jw%06x)' % (ea, name, ea) for ea, name, e in done))
    open(os.path.join(gen_dir, "jwhole_list.h"), "w", newline="\n").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
