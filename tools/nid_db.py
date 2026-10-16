import glob
import hashlib
import json
import os
import re
import sys

SUFFIX = bytes.fromhex("6759659904250490566427499489741A")
MODULE_IF = re.compile(r'if \(((?:_module == "[^"]+"(?: \|\| )?)+)\) switch')
MODULE_NAME = re.compile(r'"([^"]+)"')
CASE = re.compile(r'case (0x[0-9a-fA-F]+): return "([^"]+)";')
REG = re.compile(r'REG_(FUNC|HIDDEN_FUNC|FNID|VAR|VNID)\(\s*(\w+)\s*,\s*("([^"]+)"|0x[0-9a-fA-F]+|\w+)\s*(?:,\s*(\w+))?')


def nid(name):
    return int.from_bytes(hashlib.sha1(name.encode() + SUFFIX).digest()[:4], "little")


def build(rpcs3_cell):
    db = {}

    def add(mod, n, name):
        db.setdefault(mod, {}).setdefault(f"{n:08X}", name)

    for path in [os.path.join(rpcs3_cell, "PPUFunction.cpp")]:
        mods = []
        for line in open(path, encoding="utf-8"):
            m = MODULE_IF.search(line)
            if m:
                mods = MODULE_NAME.findall(m.group(1))
                continue
            c = CASE.search(line)
            if c and mods:
                for mod in mods:
                    add(mod, int(c.group(1), 16), c.group(2))
    for path in glob.glob(os.path.join(rpcs3_cell, "Modules", "*.cpp")) + glob.glob(os.path.join(rpcs3_cell, "lv2", "*.cpp")):
        text = open(path, encoding="utf-8", errors="replace").read()
        aliases = {}
        for block in re.split(r"(?=DECLARE\(ppu_module_manager::)", text):
            d = re.search(r"DECLARE\(ppu_module_manager::(\w+)\)\(\"([^\"]+)\"", block)
            if d:
                aliases[d.group(1)] = d.group(2)
        for kind, var, arg, quoted, func in REG.findall(text):
            mod = aliases.get(var, var)
            if kind in ("FUNC", "HIDDEN_FUNC", "VAR"):
                add(mod, nid(arg), arg)
            elif quoted:
                add(mod, nid(quoted), quoted)
            elif arg.startswith("0x"):
                add(mod, int(arg, 16), func or arg)
    return db


if __name__ == "__main__":
    db = build(sys.argv[1])
    json.dump(db, open(sys.argv[2], "w"), indent=0, sort_keys=True)
    print(len(db), "modules", sum(len(v) for v in db.values()), "names")
