import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "renjob"))
sys.path.insert(0, HERE)
import rj_cgen as G
from mp_jobs import JOBS, ROOT
from mp_kernels import KERNELS

RUNTIME = os.path.join(ROOT, "runtime", "src", "spu", "meshprep")


def load_icalls(job):
    j = JOBS[job]
    path = os.path.join(HERE, "icalls_%s.txt" % job)
    out = {}
    if os.path.exists(path):
        for line in open(path):
            toks = line.split()
            if not toks:
                continue
            ts = [int(t.split(":")[0], 16) for t in toks[1:]]
            ts = [t for t in ts if 0x4030 <= t < j["code_end"]]
            if ts:
                out[int(toks[0], 16)] = ts
    return out


def configure(job):
    j = JOBS[job]
    G.configure(IMG_EA=j["ea"], IMG_SIZE=j["size"], CODE_END=j["code_end"], ENTRY_ROOTS=(j["entry"],), INLINE_EXCEPT=tuple(j.get("inline_except", ())),
                OPS_INCLUDE="../renjob/renjob_ops.h", BASE_NS="renjob::xo", OPS_NS="renjob::xo::opz_%s" % j["short"], ROOTS_MACRO="MP_ROOTS_%s" % j["short"].upper(),
                PC_COUNTS_PATH=os.path.join(HERE, "pc_counts_%s.txt" % job), KERNELS=KERNELS.get(job, {}), ICALLS=load_icalls(job), RECURSION_LIMIT=512, LOOP_BUDGET=True)


def run(job, argv):
    configure(job)
    sys.argv = [sys.argv[0]] + argv
    G.NOCALL.clear()
    G.PC_COUNTS = None
    G.main()


def main():
    job = sys.argv[1]
    if job == "runtime":
        jobs = sys.argv[2:] or list(JOBS)
        for jb in jobs:
            roots = os.path.join(HERE, "roots_%s.txt" % jb)
            spec = open(roots).read().strip() if os.path.exists(roots) else ""
            ns = "renjob::xo::%s" % JOBS[jb]["short"]
            run(jb, [spec, os.path.join(RUNTIME, "%s_gen.cpp" % jb), os.path.join(RUNTIME, "%s_gen.h" % jb), ns])
        return
    run(job, sys.argv[2:])


if __name__ == "__main__":
    main()
