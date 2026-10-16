import re
import sys

ARRAYS = ["ppu_vrefp_segments", "ppu_vrsqrtefp_segments", "ppu_vlogefp_segments", "ppu_vexptefp_segments"]
FUNCS = ["ppu_vnormalize", "ppu_vrefp", "ppu_vrsqrtefp", "ppu_vlogefp", "ppu_vexptefp"]


def array(text, name):
    m = re.search(name + r"\[[^\]]*\]\s*=\s*\{(.*?)\};", text, re.S)
    return [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)", m.group(1))]


def block(text, start_pat):
    m = re.search(start_pat, text)
    i = text.index("{", m.end() - 1 if text[m.end() - 1] == "{" else m.end())
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                end = j + 1
                if text[end:end + 1] == ";":
                    end += 1
                return text[m.start():end]
    raise ValueError(start_pat)


def strip_comments(code):
    code = re.sub(r"/\*.*?\*/", "", code, flags=re.S)
    return "\n".join(line for line in (re.sub(r"//.*$", "", l).rstrip() for l in code.splitlines()) if line.strip())


def main(common_h, out_h):
    text = open(common_h, encoding="utf-8").read()
    fres = array(text, "ppu_fres_mantissas")
    frsqrte = array(text, "ppu_frsqrte_mantissas")
    assert len(fres) == 128 and len(frsqrte) == 16
    parts = []
    for name in ARRAYS:
        parts.append(strip_comments(block(text, r"constexpr\s+u\d+\s+" + name + r"\s*\[")))
    for name in FUNCS:
        parts.append(strip_comments(block(text, r"constexpr\s+\w+\s+" + name + r"\s*\(")))
    with open(out_h, "w") as f:
        f.write("#pragma once\n#include <bit>\n#include <cstdint>\n\nnamespace ppu::tables {\n\n")
        f.write("using u16 = uint16_t;\nusing u32 = uint32_t;\nusing s32 = int32_t;\nusing s64 = int64_t;\nusing f32 = float;\n\n")
        f.write("inline constexpr uint32_t fres_mantissa[128] = {" + ", ".join(f"0x{v:08x}u" for v in fres) + "};\n")
        f.write("inline constexpr uint32_t frsqrte_mantissa[16] = {" + ", ".join(f"0x{v:08x}u" for v in frsqrte) + "};\n\n")
        f.write("\n\n".join(parts))
        f.write("\n\n}\n")
    print(f"wrote {out_h}")


if __name__ == "__main__":
    main(*sys.argv[1:3])
