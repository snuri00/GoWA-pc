import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from sb_edits import EDITS

ROOTS = "5d90,6ac8,7040,73e0,b5f0,b600,b670"
ORDER = ("f_05d90", "f_06ac8", "f_07040", "f_073e0", "f_0b5f0", "f_0b600", "f_0b670")
RENAME = {"f_06ac8": "f_06ac8_gen"}


def functions(lines):
    out = {}
    cur = None
    for line in lines:
        m = re.match(r"void (f_[0-9a-f]+)\(V\* R, uint8_t\* ls, Ctl& ctl\) \{$", line)
        if m:
            cur = m.group(1)
            out[cur] = [line]
            continue
        if cur:
            out[cur].append(line)
            if line == "}":
                cur = None
    return out


def main(out_path):
    tmp = out_path + ".tmp"
    subprocess.run([sys.executable, os.path.join(HERE, "spu_cgen.py"), ROOTS, tmp], check=True, stdout=subprocess.DEVNULL)
    funcs = functions(open(tmp).read().split("\n"))
    os.remove(tmp)
    blocks = []
    for name in ORDER:
        lines = list(funcs[name])
        for idx, n, rep in sorted(EDITS.get(name, ()), key=lambda e: -e[0]):
            lines[idx:idx + n] = rep
        if name in RENAME:
            lines[0] = lines[0].replace(name + "(", RENAME[name] + "(")
        blocks.append("\n".join(lines))
    open(out_path, "w", newline="\n").write("\n\n".join(blocks) + "\n")


if __name__ == "__main__":
    main(sys.argv[1])
