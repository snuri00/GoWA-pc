import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tests", "renjob"))
sys.path.insert(0, HERE)

import rj_cgen as G
from sc_kernels import KERNELS
import sc_indirect

IMAGES = {
    "mp3": dict(ea=0x7e1c00, size=0x1c0a0, code_end=0x1b6d0, macro="SC_MP3_ROOTS", ns="renjob::xo::scream_mp3"),
    "aux": dict(ea=0x7bfc80, size=0x20bc0, code_end=0x204d0, macro="SC_AUX_ROOTS", ns="renjob::xo::scream_aux"),
}


def setup(name):
    c = IMAGES[name]
    G.configure(IMG_EA=c["ea"], IMG_LS=0x4000, IMG_SIZE=c["size"], CODE_END=c["code_end"], ENTRY_ROOTS=(), INLINE_EXCEPT=(), OPS_INCLUDE="scream_ops.h", BASE_NS="renjob::xo",
                OPS_NS=c["ns"] + "::opz", ROOTS_MACRO=c["macro"], PC_COUNTS_PATH=os.path.join(HERE, "pc_counts_%s.txt" % name), PC_COUNTS=None, KERNELS=dict(KERNELS.get(name, {})))
    G.NOCALL.clear()
    G.RR3.update({"dfa", "dfs", "dfm"})
    G.RR2.update({"fesd", "frds"})
    sc_indirect.install()
    return c


def gen_all():
    import subprocess
    for name in IMAGES:
        out = os.path.join(ROOT, "runtime", "src", "spu", "scream", "%s_gen" % name)
        subprocess.run([sys.executable, os.path.abspath(__file__), name, "@" + os.path.join(HERE, "roots_%s.txt" % name), out + ".cpp", out + ".h"], check=True, stdout=subprocess.DEVNULL)


def main():
    if sys.argv[1] == "all":
        gen_all()
        return
    name = sys.argv[1]
    c = setup(name)
    args = sys.argv[2:]
    if args[0] != "ops" and len(args) < 4:
        args = args + [c["ns"]]
    sys.argv = [sys.argv[0]] + args
    G.main()
    if args[0] != "ops" and os.environ.get("SC_LOOPS", "1") == "1":
        import sc_loops
        sc_loops.apply(args[1], len(args) > 4)


if __name__ == "__main__":
    main()
