import os
import re
import runpy
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sc_cgen

NAMES = {"mp3": "mp3pack", "aux": "aux_master"}


def main():
    name, calls, ratio, fns = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
    sc_cgen.setup(name)
    tmp = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False)
    for line in open(calls):
        m = re.match(r"fn (\S+) +(\w+) (calls .*)", line)
        if m and m.group(1) == NAMES[name]:
            tmp.write("fn %05x %s\n" % (int(m.group(2), 16) + 0x4000, m.group(3)))
    tmp.close()
    sys.argv = [os.path.join(sc_cgen.ROOT, "tests", "renjob", "rj_plan.py"), tmp.name, ratio, fns]
    try:
        runpy.run_path(sys.argv[0], run_name="__main__")
    finally:
        os.unlink(tmp.name)


if __name__ == "__main__":
    main()
