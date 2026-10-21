import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spu_dis as D
from mp_jobs import JOBS, image


def main():
    job = sys.argv[1]
    j = JOBS[job]
    img = image(job)
    calls = {}
    chans = {}
    for pc in range(0x4030, j["code_end"], 4):
        w = struct.unpack_from(">I", img, pc - 0x4000)[0]
        s = D.dis(w, pc)
        if s.startswith("brsl ") and not s.startswith("brsl r126"):
            t = int(s.split(",")[1], 16)
            if t != pc + 4:
                calls.setdefault(t, []).append(pc)
        if s.split()[0] in ("rdch", "wrch", "rchcnt", "stop", "stopd", "bisl", "iret"):
            chans.setdefault(s.split()[0], []).append((pc, s))
    for t in sorted(calls):
        print("fn %05x called from %s" % (t, " ".join("%05x" % c for c in calls[t][:12])))
    for k, v in chans.items():
        print(k, len(v), " ".join("%05x" % p for p, _ in v[:60]))


if __name__ == "__main__":
    main()
