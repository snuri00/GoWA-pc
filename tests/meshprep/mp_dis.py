import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spu_dis as D
from mp_jobs import JOBS, image


def main():
    job = sys.argv[1]
    img = image(job)
    lo = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x4000
    hi = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x4000 + len(img)
    for pc in range(lo, min(hi, 0x4000 + len(img)), 4):
        w = struct.unpack_from(">I", img, pc - 0x4000)[0]
        print("%05x: %08x  %s" % (pc, w, D.dis(w, pc)))


if __name__ == "__main__":
    main()
