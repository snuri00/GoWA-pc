import rj_cgen as G

CMP = {"ceq", "ceqh", "ceqb", "ceqi", "ceqhi", "ceqbi", "cgt", "cgth", "cgtb", "cgti", "cgthi", "cgtbi", "clgt", "clgth", "clgtb", "clgti", "clgthi", "clgtbi", "fcgt", "fceq", "fcmgt", "fcmeq"}
NODEF = {"stqd", "stqx", "stqr", "stqa", "br", "brz", "brnz", "brhz", "brhnz", "bi", "biz", "binz", "bihz", "bihnz", "bra", "wrch", "stop", "stopd", "heqi", "hgti", "hlgti", "heq", "hgt", "hlgt", "mtspr", "fscrwr", "iret", "invalid", "nop", "lnop", "hbr", "hbrr", "hbra", "sync", "dsync"}
MAXSET = 8


def is_copy(d):
    op = d["op"]
    if op in ("ori", "ai", "shlqbyi", "rotqbyi", "xori") and d.get("imm") == 0:
        return d["ra"]
    if op == "or" and d["ra"] == d["rb"]:
        return d["ra"]
    return None


def transfer(st, pc, d):
    op = d["op"]
    if op in NODEF or "rt" not in d:
        return st
    rt = d["rt"]
    st = dict(st)
    if op == "ila":
        st[rt] = frozenset([("c", d["imm"] & 0x3FFFF)])
    elif op == "il":
        st[rt] = frozenset([("c", d["imm"] & 0xFFFFFFFF)])
    elif op in CMP or op == "fsm":
        st[rt] = frozenset(["M"])
    elif op in ("and", "or", "andc", "orc", "nand", "nor", "xor", "eqv") and st.get(d["ra"]) == frozenset(["M"]) and st.get(d["rb"]) == frozenset(["M"]):
        st[rt] = frozenset(["M"])
    elif op == "selb":
        a, b, m = st.get(d["ra"]), st.get(d["rb"]), st.get(d["rc"])
        if a is not None and b is not None and m == frozenset(["M"]) and "M" not in a and "M" not in b and len(a | b) <= MAXSET:
            st[rt] = a | b
        else:
            st.pop(rt, None)
    elif op == "brsl":
        return {}
    else:
        src = is_copy(d)
        if src is not None and src in st:
            st[rt] = st[src]
        else:
            st.pop(rt, None)
    return st


def merge(a, b):
    out = {}
    for r, v in a.items():
        w = b.get(r)
        if w is not None:
            u = v | w
            if len(u) <= MAXSET:
                out[r] = u
    return out


def succs(f, pc, d, resolved):
    op = d["op"]
    out = []
    if op in ("br", "brz", "brnz", "brhz", "brhnz"):
        t = (pc + d["imm"] * 4) & 0x3FFFC
        if t in f.insns:
            out.append(t)
        if op == "br":
            return out
    if op == "bi":
        if pc in getattr(f, "switches", {}):
            return [t for t in f.switches[pc] if t in f.insns]
        if pc in resolved:
            return [t for t in resolved[pc] if t in f.insns]
        return out
    if op in ("bra", "brasl", "stop", "stopd", "invalid", "iret"):
        return out
    if pc + 4 in f.insns:
        out.append(pc + 4)
    return out


def analyze(f, starts):
    resolved = getattr(f, "indirect", {})
    state = {}
    work = []
    for s in starts:
        state[s] = {0: frozenset(["R0"])}
        work.append(s)
    while work:
        pc = work.pop()
        d = f.insns.get(pc)
        if d is None:
            continue
        out = transfer(state[pc], pc, d)
        for t in succs(f, pc, d, resolved):
            if t not in state:
                state[t] = out
                work.append(t)
            else:
                m = merge(state[t], out)
                if m != state[t]:
                    state[t] = m
                    work.append(t)
    found = {}
    for pc, d in f.insns.items():
        if d["op"] != "bi" or d["ra"] == 0 or d["rb"] & 0x30 or pc in getattr(f, "switches", {}) or pc not in state:
            continue
        v = state[pc].get(d["ra"])
        if not v or "M" in v:
            continue
        tg = []
        ok = True
        for x in v:
            if x == "R0":
                continue
            a = x[1]
            if a & 3 or a < G.IMG_LS + 0x30 or a >= G.CODE_END:
                ok = False
            tg.append(a)
        if ok:
            found[pc] = (sorted(tg), "R0" in v)
    return found


_orig_discover = G.discover


def discover(entry, known_entries, stops=frozenset(), entries=None):
    f = _orig_discover(entry, known_entries, stops, entries)
    f.indirect = {}
    f.indirect_r0 = {}
    starts = list(entries) if entries else [entry]
    for _ in range(16):
        found = analyze(f, starts)
        new = []
        for pc, (tg, r0) in found.items():
            if f.indirect.get(pc) != tg:
                f.indirect[pc] = tg
                new += [t for t in tg if t not in f.insns]
            f.indirect_r0[pc] = r0
        if not new:
            break
        for t in new:
            if t in f.insns:
                continue
            g = _orig_discover(t, known_entries, stops)
            f.insns.update(g.insns)
            f.labels |= g.labels
            f.calls |= g.calls
            f.tails |= g.tails
            f.exits |= g.exits
            for k, v in getattr(g, "switches", {}).items():
                f.switches = getattr(f, "switches", {})
                f.switches[k] = v
            f.has_fail = f.has_fail or g.has_fail
    f.exits -= set(f.insns)
    if f.indirect:
        f.switches = getattr(f, "switches", {})
        for pc, tg in f.indirect.items():
            f.switches[pc] = tg
            f.labels |= set(tg)
    return f


_orig_xf = G.xf


NORETURN = set()


def noreturn_seeds():
    out = set()
    for pc in range(G.IMG_LS + 0x30, G.CODE_END - 4, 4):
        d = G.decode(pc)
        if d["op"] == "brsl" and d["rt"] == 0 and G.decode(pc + 4)["op"] in ("stop", "stopd"):
            t = (pc + d["imm"] * 4) & 0x3FFFC
            if t != pc + 4:
                out.add(t)
    return out


def after_noreturn(f, pc):
    if pc - 4 not in f.insns and pc - 4 not in f.exits:
        return False
    if f.multi and pc in f.multi:
        return False
    for q, e in f.insns.items():
        if e["op"] in ("br", "brz", "brnz", "brhz", "brhnz") and ((q + e["imm"] * 4) & 0x3FFFC) == pc:
            return False
    for tg in getattr(f, "switches", {}).values():
        if pc in tg:
            return False
    d = G.decode(pc - 4)
    if d["op"] != "brsl" or d["rt"] != 0:
        return False
    t = (pc - 4 + d["imm"] * 4) & 0x3FFFC
    return t in NORETURN and (t in G.NOCALL or pc - 4 in f.exits)


def xf(f, pc):
    if pc in getattr(f, "indirect", {}):
        return "@RET@" if f.indirect_r0.get(pc) else "__builtin_unreachable()"
    if G.decode(pc)["op"] in ("stop", "stopd") and after_noreturn(f, pc):
        return "EXIT(0x%05x)" % (pc - 4)
    return _orig_xf(f, pc)




def r0_dead(x):
    decode = G.decode
    seen = set()
    work = [x]
    while work:
        pc = work.pop()
        while pc not in seen:
            seen.add(pc)
            d = decode(pc)
            op = d["op"]
            if op in G.SKIP:
                pc += 4
                continue
            if 0 in G.reads_of(d):
                return False
            if G.writes_r0(d):
                break
            if op == "br":
                pc = (pc + d["imm"] * 4) & 0x3FFFC
                continue
            if op in ("brz", "brnz", "brhz", "brhnz"):
                work.append((pc + d["imm"] * 4) & 0x3FFFC)
            if op == "brsl":
                if d["rt"] == 0:
                    break
            if op == "bi":
                prev, prev2 = decode(pc - 4), decode(pc - 8)
                if prev["op"] == "ai" and prev["ra"] == d["ra"] and prev["imm"] == 8 and prev2["op"] == "brsl" and prev2["rt"] == d["ra"] and (pc - 8 + prev2["imm"] * 4) == pc - 4:
                    pc += 4
                    continue
                if prev["op"] == "ila" and prev["rt"] == d["ra"] and prev["imm"] == pc + 4:
                    pc += 4
                    continue
                if d["ra"] != 0 and d["rb"] & 0x30 == 0 and G.jump_table(pc):
                    work.extend(G.jump_table(pc))
                    break
                return False
            if op in ("bisl", "bra", "brasl", "stop", "stopd", "invalid", "iret", "bisled"):
                return False
            pc += 4
    return True


def install():
    G.discover = discover
    G.xf = xf
    G.r0_dead = r0_dead
    NORETURN.clear()
    NORETURN.update(noreturn_seeds())
    G.NOCALL.update(NORETURN)
