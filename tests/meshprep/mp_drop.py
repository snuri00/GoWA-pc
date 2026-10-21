import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    drop = {int(x, 16) for x in sys.argv[1].split(",") if x}
    for job in ("meshprep", "primbuilder", "groupvis", "sortprim"):
        path = os.path.join(HERE, "roots_%s.txt" % job)
        toks = [t for t in open(path).read().strip().split(",") if t]
        keep = [t for t in toks if int(t.split(":")[0], 16) not in drop]
        open(path, "w").write(",".join(keep) + "\n")
        print(job, len(toks), "->", len(keep))


if __name__ == "__main__":
    main()
