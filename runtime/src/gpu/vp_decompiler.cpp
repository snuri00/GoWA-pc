#include "vp_decompiler.h"

#include <cstdio>
#include <set>
#include <sstream>
#include <vector>

namespace gpu {

namespace {

constexpr uint32_t SLOTS = 544;

struct Insn {
    uint32_t d0, d1, d2, d3;
    uint32_t vec_op() const { return (d1 >> 22) & 0x1F; }
    uint32_t sca_op() const { return (d1 >> 27) & 0x1F; }
    bool end() const { return d3 & 1; }
    uint32_t target() const { return (((d0 >> 23) & 1) << 9) | ((d2 & 0x3F) << 3) | ((d3 >> 29) & 7); }
};

bool is_flow(uint32_t sca) { return sca == 0x08 || sca == 0x09 || sca == 0x0A || sca == 0x0B || sca == 0x0C || sca == 0x11 || sca == 0x12; }

const char* lane(uint32_t i) {
    static const char* l[4] = {"x", "y", "z", "w"};
    return l[i & 3];
}

class Decompiler {
public:
    Decompiler(const uint32_t* ucode, uint32_t start) : code_(ucode), start_(start) {}

    VertexProgramInfo run() {
        VertexProgramInfo info;
        if (!discover()) return info;
        std::ostringstream body;
        if (has_flow_) {
            body << "    int pc = " << start_ << ";\n    int stk[8];\n    int sp = 0;\n    int guard = 0;\n";
            body << "    while (guard++ < 512) {\n        switch (pc) {\n";
            for (uint32_t s : slots_) {
                body << "        case " << s << ": {\n";
                emit(body, s, "            ");
                body << "        }\n";
            }
            body << "        default: break;\n        }\n        break;\n    }\n";
        } else {
            for (uint32_t s : order_) emit(body, s, "    ");
        }
        std::ostringstream src;
        src << "#version 450\n";
        for (uint32_t i = 0; i < 16; i++) {
            if (inputs_ & (1u << i)) src << "layout(location = " << i << ") in vec4 v" << i << ";\n";
        }
        src << "layout(std140, set = 0, binding = 0) uniform VertexConstants { vec4 c[468]; };\n";
        src << "layout(std140, set = 0, binding = 1) uniform VertexState { vec4 vp_scale; vec4 vp_offset; vec4 surface; uvec4 flags; };\n";
        for (uint32_t t = 0; t < 4; t++) {
            if (textures_ & (1u << t)) src << "layout(set = 0, binding = " << (16 + t) << ") uniform sampler2D vtex" << t << ";\n";
        }
        src << "layout(location = 1) out vec4 out_col0;\nlayout(location = 2) out vec4 out_col1;\nlayout(location = 3) out vec4 out_fog;\n";
        for (uint32_t i = 0; i < 8; i++) src << "layout(location = " << (4 + i) << ") out vec4 out_tex" << i << ";\n";
        src << "layout(location = 12) out vec4 out_tex8;\nlayout(location = 13) out vec4 out_tex9;\nlayout(location = 14) out vec4 out_bfc0;\nlayout(location = 15) out vec4 out_bfc1;\n";
        src << "out gl_PerVertex { invariant vec4 gl_Position; float gl_PointSize; };\n";
        src << "vec4 cread(int i) { return (i >= 0 && i < 468) ? c[i] : vec4(0.0); }\n";
        src << "bvec4 cctest(vec4 v, int cond) {\n"
               "    bvec4 lt = lessThan(v, vec4(0.0)); bvec4 eq = equal(v, vec4(0.0)); bvec4 gt = greaterThan(v, vec4(0.0));\n"
               "    bvec4 r = bvec4(false);\n"
               "    if ((cond & 1) != 0) r = bvec4(uvec4(r) | uvec4(lt));\n"
               "    if ((cond & 2) != 0) r = bvec4(uvec4(r) | uvec4(eq));\n"
               "    if ((cond & 4) != 0) r = bvec4(uvec4(r) | uvec4(gt));\n"
               "    return r;\n}\n";
        src << "void main() {\n";
        src << "    vec4 R[32];\n    for (int i = 0; i < 32; i++) R[i] = vec4(0.0);\n";
        src << "    vec4 o[16];\n    for (int i = 0; i < 16; i++) o[i] = vec4(0.0, 0.0, 0.0, 1.0);\n";
        src << "    ivec4 A[2] = ivec4[2](ivec4(0), ivec4(0));\n    vec4 CC[2] = vec4[2](vec4(0.0), vec4(0.0));\n";
        src << body.str();
        src << "    uint omask = flags.x;\n";
        src << "    out_col0 = (omask & 1u) != 0u ? o[1] : vec4(0.0, 0.0, 0.0, 1.0);\n";
        src << "    out_col1 = (omask & 2u) != 0u ? o[2] : vec4(0.0, 0.0, 0.0, 1.0);\n";
        src << "    bool two_side = (flags.y & 1u) != 0u;\n";
        src << "    out_bfc0 = two_side && (omask & 4u) != 0u ? o[3] : out_col0;\n";
        src << "    out_bfc1 = two_side && (omask & 8u) != 0u ? o[4] : out_col1;\n";
        src << "    out_fog = (omask & 16u) != 0u ? o[5].xxxx : vec4(0.0);\n";
        for (uint32_t i = 0; i < 8; i++) {
            src << "    out_tex" << i << " = (omask & " << (1u << (14 + i)) << "u) != 0u ? o[" << (7 + i) << "] : vec4(0.0, 0.0, 0.0, 1.0);\n";
        }
        src << "    out_tex8 = (omask & 4096u) != 0u ? o[15] : vec4(0.0, 0.0, 0.0, 1.0);\n";
        src << "    out_tex9 = (omask & 8192u) != 0u ? o[6] : vec4(0.0, 0.0, 0.0, 1.0);\n";
        src << "    gl_PointSize = (omask & 32u) != 0u ? o[6].x : uintBitsToFloat(flags.z);\n";
        src << "    vec4 p = o[0];\n";
        src << "    vec2 xy = (vp_scale.xy * p.xy + (vp_offset.xy - surface.xy * 0.5) * p.w) * surface.zw * 2.0;\n";
        src << "    gl_Position = vec4(xy, vp_scale.z * p.z + vp_offset.z * p.w, p.w);\n";
        src << "}\n";
        info.glsl = src.str();
        info.input_mask = inputs_;
        info.texture_mask = textures_;
        info.valid = true;
        return info;
    }

private:
    const uint32_t* code_;
    uint32_t start_;
    std::set<uint32_t> slots_;
    std::vector<uint32_t> order_;
    bool has_flow_ = false;
    uint32_t inputs_ = 0;
    uint32_t textures_ = 0;

    Insn at(uint32_t s) const { return {code_[s * 4], code_[s * 4 + 1], code_[s * 4 + 2], code_[s * 4 + 3]}; }

    bool discover() {
        std::vector<uint32_t> work{start_};
        while (!work.empty()) {
            uint32_t s = work.back();
            work.pop_back();
            while (s < SLOTS && !slots_.count(s)) {
                slots_.insert(s);
                order_.push_back(s);
                Insn i = at(s);
                uint32_t sca = i.sca_op();
                if (is_flow(sca)) {
                    has_flow_ = true;
                    if (sca != 0x08 && sca != 0x0B && sca != 0x0C) work.push_back(i.target());
                }
                if (i.end()) break;
                s++;
            }
        }
        return !order_.empty();
    }

    std::string src(const Insn& i, uint32_t n) {
        uint32_t raw = 0;
        if (n == 0) raw = ((i.d1 & 0xFF) << 9) | (i.d2 >> 23);
        else if (n == 1) raw = (i.d2 >> 6) & 0x1FFFF;
        else raw = ((i.d2 & 0x3F) << 11) | (i.d3 >> 21);
        uint32_t type = raw & 3;
        std::string base;
        if (type == 1) {
            base = "R[" + std::to_string((raw >> 2) & 0x3F) + "]";
        } else if (type == 2) {
            uint32_t in = (i.d1 >> 8) & 0xF;
            inputs_ |= 1u << in;
            base = "v" + std::to_string(in);
        } else if (type == 3) {
            uint32_t ci = (i.d1 >> 12) & 0x3FF;
            if (i.d3 & 2) {
                uint32_t a = (i.d0 >> 24) & 1;
                base = "cread((" + std::to_string(ci) + " + A[" + std::to_string(a) + "]." + lane(i.d0 & 3) + ") & 511)";
            } else {
                base = "cread(" + std::to_string(ci) + ")";
            }
        } else {
            base = "vec4(0.0)";
        }
        std::string sw = std::string(".") + lane(raw >> 14) + lane(raw >> 12) + lane(raw >> 10) + lane(raw >> 8);
        std::string v = "(" + base + ")" + sw;
        bool absf = n == 0 ? (i.d0 >> 21) & 1 : n == 1 ? (i.d0 >> 22) & 1 : (i.d0 >> 23) & 1;
        if (n == 2 && is_flow(i.sca_op())) absf = false;
        if (absf) v = "abs(" + v + ")";
        if ((raw >> 16) & 1) v = "(-" + v + ")";
        return v;
    }

    std::string vec_expr(const Insn& i, uint32_t op) {
        switch (op) {
        case 0x01: return src(i, 0);
        case 0x02: return src(i, 0) + " * " + src(i, 1);
        case 0x03: return src(i, 0) + " + " + src(i, 2);
        case 0x04: return "fma(" + src(i, 0) + ", " + src(i, 1) + ", " + src(i, 2) + ")";
        case 0x05: return "vec4(dot(" + src(i, 0) + ".xyz, " + src(i, 1) + ".xyz))";
        case 0x06: return "vec4(dot(" + src(i, 0) + ".xyz, " + src(i, 1) + ".xyz) + " + src(i, 1) + ".w)";
        case 0x07: return "vec4(dot(" + src(i, 0) + ", " + src(i, 1) + "))";
        case 0x08: return "vec4(1.0, " + src(i, 0) + ".y * " + src(i, 1) + ".y, " + src(i, 0) + ".z, " + src(i, 1) + ".w)";
        case 0x09: return "min(" + src(i, 0) + ", " + src(i, 1) + ")";
        case 0x0A: return "max(" + src(i, 0) + ", " + src(i, 1) + ")";
        case 0x0B: return "vec4(lessThan(" + src(i, 0) + ", " + src(i, 1) + "))";
        case 0x0C: return "vec4(greaterThanEqual(" + src(i, 0) + ", " + src(i, 1) + "))";
        case 0x0E: return "fract(" + src(i, 0) + ")";
        case 0x0F: return "floor(" + src(i, 0) + ")";
        case 0x10: return "vec4(equal(" + src(i, 0) + ", " + src(i, 1) + "))";
        case 0x11: return "vec4(0.0)";
        case 0x12: return "vec4(greaterThan(" + src(i, 0) + ", " + src(i, 1) + "))";
        case 0x13: return "vec4(lessThanEqual(" + src(i, 0) + ", " + src(i, 1) + "))";
        case 0x14: return "vec4(notEqual(" + src(i, 0) + ", " + src(i, 1) + "))";
        case 0x15: return "vec4(1.0)";
        case 0x16: return "sign(" + src(i, 0) + ")";
        case 0x19: {
            uint32_t t = (i.d2 >> 8) & 3;
            textures_ |= 1u << t;
            return "textureLod(vtex" + std::to_string(t) + ", " + src(i, 0) + ".xy, 0.0)";
        }
        default: return "";
        }
    }

    std::string sca_expr(const Insn& i, uint32_t op) {
        std::string x = src(i, 2) + ".x";
        switch (op) {
        case 0x01: return "vec4(" + x + ")";
        case 0x02: return "vec4(1.0 / " + x + ")";
        case 0x03: return "vec4(sign(" + x + ") * clamp(1.0 / abs(" + x + "), 5.42101e-20, 1.884467e19))";
        case 0x04: return "vec4(inversesqrt(abs(" + x + ")))";
        case 0x05: return "vec4(exp2(floor(" + x + ")), fract(" + x + "), exp2(" + x + "), 1.0)";
        case 0x06: return "vec4(floor(log2(abs(" + x + "))), abs(" + x + ") / exp2(floor(log2(abs(" + x + ")))), log2(abs(" + x + ")), 1.0)";
        case 0x07: {
            std::string t = src(i, 2);
            return "vec4(1.0, max(" + t + ".x, 0.0), " + t + ".x > 0.0 ? pow(max(" + t + ".y, 1e-10), clamp(" + t + ".w, -128.0, 128.0)) : 0.0, 1.0)";
        }
        case 0x0D: return "vec4(log2(" + x + "))";
        case 0x0E: return "vec4(exp2(" + x + "))";
        case 0x0F: return "vec4(sin(" + x + "))";
        case 0x10: return "vec4(cos(" + x + "))";
        default: return "";
        }
    }

    std::string cond_mask(const Insn& i) {
        uint32_t cond = (i.d0 >> 10) & 7;
        uint32_t sel = (i.d0 >> 25) & 1;
        std::string sw = std::string(".") + lane(i.d0 >> 8) + lane(i.d0 >> 6) + lane(i.d0 >> 4) + lane(i.d0 >> 2);
        return "cctest(CC[" + std::to_string(sel) + "]" + sw + ", " + std::to_string(cond) + ")";
    }

    void masked_write(std::ostream& o, const std::string& ind, const std::string& dst, const std::string& val, uint32_t mask, bool cond) {
        for (uint32_t l = 0; l < 4; l++) {
            if (!(mask & (8u >> l))) continue;
            if (cond) o << ind << "if (ct." << lane(l) << ") ";
            else o << ind;
            o << dst << "." << lane(l) << " = " << val << "." << lane(l) << ";\n";
        }
    }

    void emit(std::ostream& o, uint32_t s, const std::string& ind) {
        Insn i = at(s);
        uint32_t vop = i.vec_op(), sop = i.sca_op();
        bool cond = (i.d0 >> 13) & 1;
        uint32_t condv = (i.d0 >> 10) & 7;
        bool sat = (i.d0 >> 26) & 1;
        uint32_t cc_sel = (i.d0 >> 25) & 1;
        bool vec_result = (i.d0 >> 30) & 1;
        uint32_t dst = (i.d3 >> 2) & 0x1F;
        uint32_t vtmp = (i.d0 >> 15) & 0x3F;
        uint32_t stmp = (i.d3 >> 7) & 0x3F;
        uint32_t vmask = (i.d3 >> 13) & 0xF;
        uint32_t smask = (i.d3 >> 17) & 0xF;
        bool upd_v = (i.d0 >> 14) & 1;
        bool upd_s = (i.d0 >> 29) & 1;
        o << ind << "{\n";
        std::string in = ind + "    ";
        if (cond && condv == 0) {
            o << ind << "}\n";
            if (has_flow_) {
                if (i.end()) o << ind << "pc = -1; break;\n";
                else if (!slots_.count(s + 1)) o << ind << "pc = " << (s + 1) << "; continue;\n";
            }
            return;
        }
        if (cond) o << in << "bvec4 ct = " << cond_mask(i) << ";\n";
        std::string vexpr = vop == 0x0D ? "" : vec_expr(i, vop);
        std::string sexpr = is_flow(sop) ? "" : sca_expr(i, sop);
        if (!vexpr.empty()) o << in << "vec4 vr = " << (sat ? "clamp(" + vexpr + ", 0.0, 1.0)" : vexpr) << ";\n";
        if (vop == 0x0D) o << in << "ivec4 ar = ivec4(floor(" << src(i, 0) << "));\n";
        if (!sexpr.empty()) o << in << "vec4 sr = " << (sat ? "clamp(" + sexpr + ", 0.0, 1.0)" : sexpr) << ";\n";
        if (vop == 0x0D) {
            std::string a = "A[" + std::to_string(vtmp & 1) + "]";
            for (uint32_t l = 0; l < 4; l++) {
                if (vmask & (8u >> l)) o << in << (cond ? std::string("if (ct.") + lane(l) + ") " : "") << a << "." << lane(l) << " = ar." << lane(l) << ";\n";
            }
        } else if (!vexpr.empty()) {
            bool wrote = false;
            if (vtmp != 0x3F) {
                masked_write(o, in, "R[" + std::to_string(vtmp) + "]", "vr", vmask, cond);
                wrote = true;
            }
            if (vec_result && dst < 16) {
                masked_write(o, in, "o[" + std::to_string(dst) + "]", "vr", vmask, cond);
                wrote = true;
            }
            if (!wrote || upd_v) masked_write(o, in, "CC[" + std::to_string(cc_sel) + "]", "vr", vmask, cond);
        }
        if (!sexpr.empty()) {
            bool wrote = false;
            if (stmp != 0x3F) {
                masked_write(o, in, "R[" + std::to_string(stmp) + "]", "sr", smask, cond);
                wrote = true;
            } else if (!vec_result && dst < 16) {
                masked_write(o, in, "o[" + std::to_string(dst) + "]", "sr", smask, cond);
                wrote = true;
            }
            if (!wrote || upd_s) masked_write(o, in, "CC[" + std::to_string(cc_sel) + "]", "sr", smask, cond);
        }
        if (has_flow_ && is_flow(sop)) {
            std::string take = condv == 7 ? "true" : condv == 0 ? "false" : "any(" + cond_mask(i) + ")";
            uint32_t t = i.target();
            switch (sop) {
            case 0x09: o << in << "if (" << take << ") { pc = " << t << "; continue; }\n"; break;
            case 0x0A: o << in << "if (" << take << " && sp < 8) { stk[sp++] = " << (s + 1) << "; pc = " << t << "; continue; }\n"; break;
            case 0x0C: o << in << "if (" << take << ") { if (sp == 0) { pc = -1; break; } pc = stk[--sp]; continue; }\n"; break;
            case 0x11:
            case 0x12: {
                uint32_t bit = (i.d3 >> 23) & 0x1F;
                uint32_t want = (i.d3 >> 28) & 1;
                std::string test = "((flags.w >> " + std::to_string(bit) + "u) & 1u) == " + std::to_string(want) + "u";
                if (sop == 0x11) o << in << "if (" << test << ") { pc = " << t << "; continue; }\n";
                else o << in << "if (" << test << " && sp < 8) { stk[sp++] = " << (s + 1) << "; pc = " << t << "; continue; }\n";
                break;
            }
            default: break;
            }
        }
        o << ind << "}\n";
        if (has_flow_) {
            if (i.end()) o << ind << "pc = -1; break;\n";
            else if (!slots_.count(s + 1)) o << ind << "pc = " << (s + 1) << "; continue;\n";
        }
    }
};

}

VertexProgramInfo decompile_vertex_program(const uint32_t* ucode, uint32_t start_slot) {
    if (start_slot >= SLOTS) return {};
    return Decompiler(ucode, start_slot).run();
}

}
