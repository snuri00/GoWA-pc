import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tests", "pm"))
import wws_job_cgen as W

JOBS = (
    ("meshprep", "mp", 0x74B100, 0x4440),
    ("primbuilder", "pb", 0x6F8800, 0xCE30),
    ("groupvis", "gv", 0x793500, 0x7080),
    ("sortprim", "sp", 0x6F4A00, 0x2790),
)


def native_roots(job):
    text = open(os.path.join(ROOT, "tests", "meshprep", "roots_%s.txt" % job)).read().strip()
    return frozenset(int(item.split(":")[0], 16) for item in text.split(",") if item)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ["GOWA_GEN_DIR"], "mpjob")
    want = set(sys.argv[2:])
    os.makedirs(out, exist_ok=True)
    for job, short, ea, size in JOBS:
        if want and job not in want:
            continue
        W.generate(ea, size, "pm::gen::%sj" % short, os.path.join(out, "%sj_gen.cpp" % short), os.path.join(out, "%sj_gen.h" % short), exits=native_roots(job))
        print(job, file=sys.stderr)
