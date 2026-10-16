import json
import re
import sys

ENTRY = re.compile(
    r"sk_(\w+?)_arr\.emplace_back\(\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*(KEY_\w+),\s*"
    r'"([0-9A-Fa-f]*)",\s*"([0-9A-Fa-f]*)"'
)


def main(src, dst):
    text = open(src, encoding="utf-8").read()
    keys = [
        {"table": m[0], "ver_start": m[1], "ver_end": m[2], "revision": int(m[3], 16), "type": m[4], "erk": m[5], "riv": m[6]}
        for m in ENTRY.findall(text)
    ]
    json.dump(keys, open(dst, "w"), indent=1)
    print(len(keys), "keys", sorted({k["table"] for k in keys}))


if __name__ == "__main__":
    main(*sys.argv[1:3])
