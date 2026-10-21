import re
import sys


def main():
    roots_path, unit_path = sys.argv[1], sys.argv[2]
    minrun = float(sys.argv[3]) if len(sys.argv) > 3 else 30
    per = {}
    for line in open(unit_path):
        m = re.match(r"fn (\w+): hits (\d+) ran (\d+).*spu-insns (\d+) \(([\d.]+)/run\)", line)
        if m:
            per[int(m.group(1), 16)] = float(m.group(5))
    toks = [t for t in open(roots_path).read().strip().split(",") if t]
    keep, drop = [], []
    for t in toks:
        pc = int(t.split(":")[0], 16)
        if per.get(pc, 0) >= minrun:
            keep.append(t)
        else:
            drop.append("%05x" % pc)
    open(roots_path, "w").write(",".join(keep) + "\n")
    print("kept %d dropped %d: %s" % (len(keep), len(drop), " ".join(drop)))


if __name__ == "__main__":
    main()
