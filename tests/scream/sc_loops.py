import re

SW = re.compile(r"^    switch \(pref\(r(\d+)\) & 0x3fffc\) \{ case 0x([0-9a-f]{5}): goto L_\2; default: \{  return; \}; \}$")
BAD = re.compile(r"goto|return|EXIT|FAIL|\b[fgm]_[0-9a-f]{5}\(|^L_|[{}]")
REG = re.compile(r"\br(\d+)\b")


def promote(lines, budget=False):
    out = list(lines)
    count = 0
    i = 0
    while i < len(out):
        m = SW.match(out[i])
        if not m:
            i += 1
            continue
        reg, tgt = m.group(1), m.group(2)
        lab = "L_%s:" % tgt
        j = i - 1
        while j >= 0 and out[j] != lab and not out[j].startswith("L_") and not out[j].startswith("RJ_ABI") and not BAD.search(out[j]):
            j -= 1
        if j < 0 or out[j] != lab:
            i += 1
            continue
        body = out[j + 1:i]
        regs = sorted({int(x) for l in body for x in REG.findall(l)} | {int(reg)})
        rn = lambda s: REG.sub(lambda mm: "l" + mm.group(1), s)
        decl = "    V " + ", ".join("l%d = r%d" % (r, r) for r in regs) + ";"
        wb = " ".join("r%d = l%d;" % (r, r) for r in regs)
        new = [lab + " {", decl, "L_%s_b:" % tgt] + [rn(l) for l in body]
        if budget:
            new.append("    if (--ctl.budget == 0) { %s FAIL(0x%s); }" % (wb, tgt))
        new.append("    if ((pref(l%s) & 0x3fffc) == 0x%s) goto L_%s_b;" % (reg, tgt, tgt))
        new.append("    %s return; }" % wb)
        out[j:i + 1] = new
        count += 1
        i = j + len(new)
    return out, count


def apply(path, budget=False):
    lines = open(path).read().split("\n")
    lines, n = promote(lines, budget)
    if budget:
        lines = [("    if (--ctl.budget == 0) FAIL(0x00000);\n" + l) if l.startswith("    switch (pref(") else l for l in lines]
    open(path, "w", newline="\n").write("\n".join(lines))
    return n
