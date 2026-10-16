import json
import re
import sys


def main(lv2_cpp, out_json):
    text = open(lv2_cpp, encoding="utf-8").read()
    start = text.index("g_ppu_syscall_table")
    body_start = text.index("{", text.index("\n", start))
    depth = 0
    for j in range(body_start, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                body = text[body_start + 1:j]
                break
    body = re.sub(r"//.*", "", body)
    entries = []
    depth = 0
    cur = ""
    for ch in body:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            entries.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        entries.append(cur.strip())
    table = {}
    for n, e in enumerate(entries):
        m = re.match(r"(?:BIND_SYSC|NULL_FUNC)\((\w+)\)", e)
        if m:
            table[n] = m.group(1)
    json.dump(table, open(out_json, "w"), indent=0)
    print(f"{len(entries)} slots, {len(table)} named")


if __name__ == "__main__":
    main(*sys.argv[1:3])
