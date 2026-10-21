import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from mp_jobs import JOBS, ELF

OTHER = {"edge": (0x737580, 0xc430), "renjob": (0x705680, 0x13580), "texcreator": (0x730800, 0x4ea0), "cloth": (0x728400, 0x8400), "shadows": (0x79c400, 0xc000),
         "waterproc": (0x71c500, 0xbeb0), "rsxwakeup": (0x748400, 0x2000), "pm": (0x7a7b00, 0x2ff0)}


def main():
    d = open(ELF, "rb").read()
    images = {k: (v["ea"], v["size"]) for k, v in JOBS.items()}
    images.update(OTHER)
    bad = 0
    for job in JOBS:
        path = os.path.join(HERE, "roots_%s.txt" % job)
        toks = [t for t in open(path).read().strip().split(",") if t]
        ea = JOBS[job]["ea"]
        for t in toks:
            lsa = int(t.split(":")[0], 16)
            sig = d[ea - 0x10000 + lsa - 0x4000: ea - 0x10000 + lsa - 0x4000 + 64]
            for name, (oea, osz) in images.items():
                if name == job:
                    continue
                if lsa - 0x4000 + 64 > osz:
                    continue
                if d[oea - 0x10000 + lsa - 0x4000: oea - 0x10000 + lsa - 0x4000 + 64] == sig:
                    print("collision %s %05x with %s" % (job, lsa, name))
                    bad += 1
    print("collisions", bad)


if __name__ == "__main__":
    main()
