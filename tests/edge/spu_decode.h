#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace spudec {

struct Insn {
    const char* name = ".word";
    int form = 0;
    int dst = -1;
    int src[4] = {-1, -1, -1, -1};
    int nsrc = 0;
    bool load = false;
    bool store = false;
    int addr_a = -1;
    int addr_b = -1;
    int32_t imm = 0;
    bool abs_addr = false;
    bool pc_rel = false;
};

enum Form { F_RRR = 1, F_RI18, F_RI10, F_RI16, F_RI8, F_RR, F_RI7 };

inline int32_t sext(uint32_t v, int bits) { return (v & (1u << (bits - 1))) ? int32_t(v) - (1 << bits) : int32_t(v); }

struct Entry {
    uint32_t op;
    const char* name;
};

inline const char* lookup(const Entry* t, size_t n, uint32_t op) {
    for (size_t i = 0; i < n; i++)
        if (t[i].op == op) return t[i].name;
    return nullptr;
}

inline bool is(const char* a, const char* b) { return a && std::string(a) == b; }
inline bool any_of(const char* n, std::initializer_list<const char*> l) {
    for (auto s : l)
        if (is(n, s)) return true;
    return false;
}

inline Insn decode(uint32_t w) {
    static const Entry rrr[] = {{0x8, "selb"}, {0xB, "shufb"}, {0xC, "mpya"}, {0xD, "fnms"}, {0xE, "fma"}, {0xF, "fms"}};
    static const Entry ri18[] = {{0x21, "ila"}, {0x08, "hbra"}, {0x09, "hbrr"}};
    static const Entry ri10[] = {{0x04, "ori"}, {0x05, "orhi"}, {0x06, "orbi"}, {0x0C, "sfi"}, {0x0D, "sfhi"}, {0x14, "andi"}, {0x15, "andhi"}, {0x16, "andbi"}, {0x1C, "ai"}, {0x1D, "ahi"}, {0x24, "stqd"}, {0x34, "lqd"}, {0x44, "xori"}, {0x45, "xorhi"}, {0x46, "xorbi"}, {0x4C, "cgti"}, {0x4D, "cgthi"}, {0x4E, "cgtbi"}, {0x4F, "hgti"}, {0x5C, "clgti"}, {0x5D, "clgthi"}, {0x5E, "clgtbi"}, {0x5F, "hlgti"}, {0x74, "mpyi"}, {0x75, "mpyui"}, {0x7C, "ceqi"}, {0x7D, "ceqhi"}, {0x7E, "ceqbi"}, {0x7F, "heqi"}};
    static const Entry ri16[] = {{0x040, "brz"}, {0x041, "stqa"}, {0x042, "brnz"}, {0x044, "brhz"}, {0x046, "brhnz"}, {0x047, "stqr"}, {0x060, "bra"}, {0x061, "lqa"}, {0x062, "brasl"}, {0x064, "br"}, {0x065, "fsmbi"}, {0x066, "brsl"}, {0x067, "lqr"}, {0x081, "il"}, {0x082, "ilhu"}, {0x083, "ilh"}, {0x0C1, "iohl"}};
    static const Entry ri8[] = {{0x1D8, "cflts"}, {0x1D9, "cfltu"}, {0x1DA, "csflt"}, {0x1DB, "cuflt"}};
    static const Entry rr[] = {{0x000, "stop"}, {0x001, "lnop"}, {0x002, "sync"}, {0x003, "dsync"}, {0x00C, "mfspr"}, {0x00D, "rdch"}, {0x00F, "rchcnt"}, {0x040, "sf"}, {0x041, "or"}, {0x042, "bg"}, {0x048, "sfh"}, {0x049, "nor"}, {0x053, "absdb"}, {0x058, "rot"}, {0x059, "rotm"}, {0x05A, "rotma"}, {0x05B, "shl"}, {0x05C, "roth"}, {0x05D, "rothm"}, {0x05E, "rotmah"}, {0x05F, "shlh"}, {0x078, "roti"}, {0x079, "rotmi"}, {0x07A, "rotmai"}, {0x07B, "shli"}, {0x07C, "rothi"}, {0x07D, "rothmi"}, {0x07E, "rotmahi"}, {0x07F, "shlhi"}, {0x0C0, "a"}, {0x0C1, "and"}, {0x0C2, "cg"}, {0x0C8, "ah"}, {0x0C9, "nand"}, {0x0D3, "avgb"}, {0x10C, "mtspr"}, {0x10D, "wrch"}, {0x128, "biz"}, {0x129, "binz"}, {0x12A, "bihz"}, {0x12B, "bihnz"}, {0x140, "stopd"}, {0x144, "stqx"}, {0x1A8, "bi"}, {0x1A9, "bisl"}, {0x1AA, "iret"}, {0x1AB, "bisled"}, {0x1AC, "hbr"}, {0x1B0, "gb"}, {0x1B1, "gbh"}, {0x1B2, "gbb"}, {0x1B4, "fsm"}, {0x1B5, "fsmh"}, {0x1B6, "fsmb"}, {0x1B8, "frest"}, {0x1B9, "frsqest"}, {0x1C4, "lqx"}, {0x1CC, "rotqbybi"}, {0x1CD, "rotqmbybi"}, {0x1CF, "shlqbybi"}, {0x1D4, "cbx"}, {0x1D5, "chx"}, {0x1D6, "cwx"}, {0x1D7, "cdx"}, {0x1D8, "rotqbi"}, {0x1D9, "rotqmbi"}, {0x1DB, "shlqbi"}, {0x1DC, "rotqby"}, {0x1DD, "rotqmby"}, {0x1DF, "shlqby"}, {0x1F0, "orx"}, {0x1F4, "cbd"}, {0x1F5, "chd"}, {0x1F6, "cwd"}, {0x1F7, "cdd"}, {0x1F8, "rotqbii"}, {0x1F9, "rotqmbii"}, {0x1FB, "shlqbii"}, {0x1FC, "rotqbyi"}, {0x1FD, "rotqmbyi"}, {0x1FF, "shlqbyi"}, {0x201, "nop"}, {0x240, "cgt"}, {0x241, "xor"}, {0x248, "cgth"}, {0x249, "eqv"}, {0x250, "cgtb"}, {0x253, "sumb"}, {0x258, "hgt"}, {0x2A5, "clz"}, {0x2A6, "xswd"}, {0x2AE, "xshw"}, {0x2B4, "cntb"}, {0x2B6, "xsbh"}, {0x2C0, "clgt"}, {0x2C1, "andc"}, {0x2C2, "fcgt"}, {0x2C3, "dfcgt"}, {0x2C4, "fa"}, {0x2C5, "fs"}, {0x2C6, "fm"}, {0x2C8, "clgth"}, {0x2C9, "orc"}, {0x2CA, "fcmgt"}, {0x2CB, "dfcmgt"}, {0x2CC, "dfa"}, {0x2CD, "dfs"}, {0x2CE, "dfm"}, {0x2D0, "clgtb"}, {0x2D8, "hlgt"}, {0x340, "addx"}, {0x341, "sfx"}, {0x342, "cgx"}, {0x343, "bgx"}, {0x346, "mpyhha"}, {0x34E, "mpyhhau"}, {0x35C, "dfma"}, {0x35D, "dfms"}, {0x35E, "dfnms"}, {0x35F, "dfnma"}, {0x398, "fscrrd"}, {0x3B8, "fesd"}, {0x3B9, "frds"}, {0x3BA, "fscrwr"}, {0x3BF, "dftsv"}, {0x3C0, "ceq"}, {0x3C2, "fceq"}, {0x3C3, "dfceq"}, {0x3C4, "mpy"}, {0x3C5, "mpyh"}, {0x3C6, "mpyhh"}, {0x3C7, "mpys"}, {0x3C8, "ceqh"}, {0x3CA, "fcmeq"}, {0x3CB, "dfcmeq"}, {0x3CC, "mpyu"}, {0x3CE, "mpyhhu"}, {0x3D0, "ceqb"}, {0x3D4, "fi"}, {0x3D8, "heq"}};
    Insn in;
    int rt = w & 0x7F, ra = (w >> 7) & 0x7F, rb = (w >> 14) & 0x7F;
    auto add = [&](int r) { in.src[in.nsrc++] = r; };
    if (const char* n = lookup(rrr, std::size(rrr), w >> 28)) {
        in.name = n;
        in.form = F_RRR;
        in.dst = (w >> 21) & 0x7F;
        add(ra);
        add(rb);
        add(w & 0x7F);
        return in;
    }
    if (const char* n = lookup(ri18, std::size(ri18), w >> 25)) {
        in.name = n;
        in.form = F_RI18;
        in.imm = (w >> 7) & 0x3FFFF;
        if (is(n, "ila")) in.dst = rt;
        return in;
    }
    if (const char* n = lookup(ri10, std::size(ri10), w >> 24)) {
        in.name = n;
        in.form = F_RI10;
        in.imm = sext((w >> 14) & 0x3FF, 10);
        if (is(n, "stqd")) {
            add(rt);
            in.store = true;
            in.addr_a = ra;
            in.imm *= 16;
        } else if (is(n, "lqd")) {
            in.dst = rt;
            in.load = true;
            in.addr_a = ra;
            in.imm *= 16;
        } else if (any_of(n, {"hgti", "hlgti", "heqi"})) {
            add(ra);
        } else {
            in.dst = rt;
            add(ra);
        }
        return in;
    }
    if (const char* n = lookup(ri16, std::size(ri16), w >> 23)) {
        in.name = n;
        in.form = F_RI16;
        in.imm = sext((w >> 7) & 0xFFFF, 16);
        if (any_of(n, {"brz", "brnz", "brhz", "brhnz"})) add(rt);
        else if (any_of(n, {"stqa", "stqr"})) {
            add(rt);
            in.store = true;
            in.abs_addr = is(n, "stqa");
            in.pc_rel = is(n, "stqr");
        } else if (any_of(n, {"lqa", "lqr"})) {
            in.dst = rt;
            in.load = true;
            in.abs_addr = is(n, "lqa");
            in.pc_rel = is(n, "lqr");
        } else if (any_of(n, {"brsl", "brasl"})) in.dst = rt;
        else if (is(n, "iohl")) {
            in.dst = rt;
            add(rt);
        } else if (any_of(n, {"il", "ilhu", "ilh", "fsmbi"})) in.dst = rt;
        return in;
    }
    if (const char* n = lookup(ri8, std::size(ri8), w >> 22)) {
        in.name = n;
        in.form = F_RI8;
        in.imm = (w >> 14) & 0xFF;
        in.dst = rt;
        add(ra);
        return in;
    }
    if (const char* n = lookup(rr, std::size(rr), w >> 21)) {
        in.name = n;
        in.form = F_RR;
        static const char* ri7[] = {"roti", "rotmi", "rotmai", "shli", "rothi", "rothmi", "rotmahi", "shlhi", "cbd", "chd", "cwd", "cdd", "rotqbii", "rotqmbii", "shlqbii", "rotqbyi", "rotqmbyi", "shlqbyi"};
        bool is_ri7 = false;
        for (auto s : ri7)
            if (is(n, s)) is_ri7 = true;
        if (is_ri7) {
            in.form = F_RI7;
            in.imm = sext((w >> 14) & 0x7F, 7);
            in.dst = rt;
            add(ra);
            return in;
        }
        if (is(n, "stqx")) {
            add(rt);
            in.store = true;
            in.addr_a = ra;
            in.addr_b = rb;
        } else if (is(n, "lqx")) {
            in.dst = rt;
            in.load = true;
            in.addr_a = ra;
            in.addr_b = rb;
        } else if (any_of(n, {"stop", "lnop", "sync", "dsync", "nop", "hbr", "stopd", "iret", "mtspr"})) {
        } else if (any_of(n, {"rdch", "rchcnt", "mfspr", "fscrrd"})) in.dst = rt;
        else if (is(n, "wrch")) add(rt);
        else if (any_of(n, {"bi", "biz", "binz", "bihz", "bihnz"})) {
            add(ra);
            if (!is(n, "bi")) add(rt);
        } else if (any_of(n, {"bisl", "bisled"})) {
            in.dst = rt;
            add(ra);
        } else if (any_of(n, {"heq", "hgt", "hlgt"})) {
            add(ra);
            add(rb);
        } else if (any_of(n, {"gb", "gbh", "gbb", "fsm", "fsmh", "fsmb", "frest", "frsqest", "orx", "clz", "xswd", "xshw", "xsbh", "cntb", "fesd", "frds", "fscrwr"})) {
            in.dst = rt;
            add(ra);
        } else if (any_of(n, {"addx", "sfx", "cgx", "bgx", "mpyhha", "mpyhhau", "dfma", "dfms", "dfnms", "dfnma"})) {
            in.dst = rt;
            add(ra);
            add(rb);
            add(rt);
        } else if (any_of(n, {"cbx", "chx", "cwx", "cdx"})) {
            in.dst = rt;
            add(ra);
            add(rb);
        } else {
            in.dst = rt;
            add(ra);
            add(rb);
        }
        return in;
    }
    return in;
}

inline std::string disasm(uint32_t w, uint32_t pc) {
    Insn in = decode(w);
    char buf[128];
    int rt = w & 0x7F, ra = (w >> 7) & 0x7F, rb = (w >> 14) & 0x7F;
    switch (in.form) {
    case F_RRR: std::snprintf(buf, sizeof buf, "%s r%d,r%d,r%d,r%d", in.name, (w >> 21) & 0x7F, ra, rb, w & 0x7F); break;
    case F_RI18: std::snprintf(buf, sizeof buf, "%s r%d,0x%x", in.name, rt, in.imm); break;
    case F_RI10: std::snprintf(buf, sizeof buf, "%s r%d,r%d,%d", in.name, rt, ra, in.imm); break;
    case F_RI16:
        if (in.pc_rel || std::string(in.name).rfind("br", 0) == 0) std::snprintf(buf, sizeof buf, "%s r%d,0x%05x", in.name, rt, (pc + in.imm * 4) & 0x3FFFF);
        else std::snprintf(buf, sizeof buf, "%s r%d,0x%x", in.name, rt, (w >> 7) & 0xFFFF);
        break;
    case F_RI8: std::snprintf(buf, sizeof buf, "%s r%d,r%d,%d", in.name, rt, ra, in.imm); break;
    case F_RI7: std::snprintf(buf, sizeof buf, "%s r%d,r%d,%d", in.name, rt, ra, in.imm); break;
    case F_RR: std::snprintf(buf, sizeof buf, "%s r%d,r%d,r%d", in.name, rt, ra, rb); break;
    default: std::snprintf(buf, sizeof buf, ".word 0x%08x", w); break;
    }
    return buf;
}

}
