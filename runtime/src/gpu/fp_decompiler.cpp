#include "fp_decompiler.h"

#include <map>
#include <sstream>

namespace gpu {

namespace {

constexpr uint32_t MAX_SLOTS = 4096;

struct Slot {
    uint32_t w[4];
    uint32_t opcode() const { return ((w[0] >> 24) & 0x3F) | (((w[2] >> 31) & 1) << 6); }
    bool end() const { return w[0] & 1; }
    bool uses_literal() const {
        for (int i = 1; i < 4; i++) {
            if ((w[i] & 3) == 2) return true;
        }
        return false;
    }
};

bool is_flow(uint32_t op) { return op >= 0x40 && op <= 0x45; }
bool is_tex(uint32_t op) { return op == 0x17 || op == 0x18 || op == 0x19 || op == 0x2F || op == 0x31 || op == 0x33 || op == 0x34; }

const char* lane(uint32_t i) {
    static const char* l[4] = {"x", "y", "z", "w"};
    return l[i & 3];
}

struct Layout {
    uint32_t start = 0;
    uint32_t length = 0;
    std::vector<uint32_t> literal_slots;
    uint32_t texture_mask = 0;
    uint64_t hash = 1469598103934665603ull;
    bool valid = false;
};

Slot read_slot(const FpWordReader& read, uint32_t s) { return {read(s * 4), read(s * 4 + 1), read(s * 4 + 2), read(s * 4 + 3)}; }

Layout layout(const FpWordReader& read) {
    Layout l;
    uint32_t s = 0;
    while (s < MAX_SLOTS) {
        Slot sl = read_slot(read, s);
        if (sl.opcode() != 0) break;
        if (sl.end()) {
            l.start = s;
            l.length = 1;
            l.valid = true;
            return l;
        }
        s++;
    }
    l.start = s;
    uint32_t p = s;
    while (p < MAX_SLOTS) {
        Slot sl = read_slot(read, p);
        uint32_t op = sl.opcode();
        for (int k = 0; k < 4; k++) {
            l.hash ^= sl.w[k];
            l.hash *= 1099511628211ull;
        }
        if (is_tex(op)) l.texture_mask |= 1u << ((sl.w[0] >> 17) & 0xF);
        bool lit = sl.uses_literal() && (!is_flow(op) || op == 0);
        p++;
        if (lit) {
            l.literal_slots.push_back(p - s);
            p++;
        }
        if (sl.end()) {
            l.length = p - s;
            l.valid = true;
            return l;
        }
    }
    return l;
}

class Decompiler {
public:
    Decompiler(const FpWordReader& read, const FragmentProgramKey& key, const Layout& l) : read_(read), key_(key), l_(l) {}

    std::string run() {
        std::map<uint32_t, uint32_t> literal_index;
        for (uint32_t i = 0; i < l_.literal_slots.size(); i++) literal_index[l_.literal_slots[i]] = i;
        std::ostringstream body;
        std::vector<std::pair<uint32_t, std::string>> closers;
        uint32_t p = 0;
        std::string ind = "    ";
        while (p < l_.length) {
            for (size_t c = closers.size(); c-- > 0;) {
                if (closers[c].first == p) {
                    if (closers[c].second == "}") ind.resize(ind.size() - 4);
                    else ind.resize(ind.size() - 4);
                    body << ind << closers[c].second << "\n";
                    if (closers[c].second != "}") ind += "    ";
                    closers.erase(closers.begin() + c);
                }
            }
            Slot sl = read_slot(read_, l_.start + p);
            uint32_t op = sl.opcode();
            bool lit = sl.uses_literal() && (!is_flow(op) || op == 0);
            current_literal_ = lit ? literal_index[p + 1] : 0;
            if (is_flow(op)) {
                emit_flow(body, sl, op, p, ind, closers);
            } else if (op != 0) {
                emit_op(body, sl, op, ind);
            }
            p += lit ? 2 : 1;
            if (sl.end()) break;
        }
        while (!closers.empty()) {
            ind.resize(ind.size() >= 8 ? ind.size() - 4 : 4);
            body << ind << "}\n";
            closers.pop_back();
        }
        return assemble(body.str());
    }

    uint32_t texture_mask() const { return used_tex_; }

private:
    const FpWordReader& read_;
    FragmentProgramKey key_;
    const Layout& l_;
    uint32_t current_literal_ = 0;
    uint32_t used_tex_ = 0;
    bool uses_kil_ = false;

    uint32_t dim(uint32_t unit) const { return (key_.texture_dims >> (unit * 2)) & 3; }

    std::string input(uint32_t attr) {
        switch (attr) {
        case 0: return "wpos()";
        case 1: return "clamp(gl_FrontFacing ? in_col0 : in_bfc0, 0.0, 1.0)";
        case 2: return "clamp(gl_FrontFacing ? in_col1 : in_bfc1, 0.0, 1.0)";
        case 3: return "fogc()";
        case 14: return "vec4(gl_FrontFacing ? 1.0 : -1.0)";
        default:
            if (attr >= 4 && attr <= 13) return "in_tex" + std::to_string(attr - 4);
            return "vec4(0.0)";
        }
    }

    std::string src(const Slot& sl, uint32_t n) {
        uint32_t w = sl.w[1 + n];
        uint32_t type = w & 3;
        std::string v;
        if (type == 0) {
            uint32_t idx = (w >> 2) & 0x3F;
            v = ((w >> 8) & 1) ? "H[" + std::to_string(idx) + "]" : "R[" + std::to_string(idx) + "]";
        } else if (type == 1) {
            v = input((sl.w[0] >> 13) & 0xF);
        } else if (type == 2) {
            v = "fc[" + std::to_string(current_literal_) + "]";
        } else {
            v = "vec4(0.0)";
        }
        v = "(" + v + ")." + lane(w >> 9) + lane(w >> 11) + lane(w >> 13) + lane(w >> 15);
        uint32_t prec = (sl.w[2] >> (19 + 3 * n)) & 7;
        bool absf = n == 0 ? (sl.w[1] >> 29) & 1 : (w >> 18) & 1;
        if (prec == 4) v = "clamp(" + v + ", 0.0, 1.0)";
        if (absf) v = "abs(" + v + ")";
        if (prec == 2) v = "clamp(" + v + ", -2.0, 2.0)";
        else if (prec == 3) v = "clamp(" + v + ", -1.0, 1.0)";
        if ((w >> 17) & 1) v = "(-" + v + ")";
        return v;
    }

    std::string tex_coord(const std::string& a, uint32_t unit) {
        switch (dim(unit)) {
        case 0: return a + ".x";
        case 1: return a + ".xy";
        default: return a + ".xyz";
        }
    }

    std::string tex(const Slot& sl, uint32_t op) {
        uint32_t unit = (sl.w[0] >> 17) & 0xF;
        used_tex_ |= 1u << unit;
        std::string s = "tex" + std::to_string(unit);
        std::string a = src(sl, 0);
        std::string r;
        switch (op) {
        case 0x17: r = "texture(" + s + ", " + tex_coord(a, unit) + ")"; break;
        case 0x18: r = "texture(" + s + ", " + tex_coord("(" + a + " / " + a + ".w)", unit) + ")"; break;
        case 0x19: r = "textureGrad(" + s + ", " + tex_coord(a, unit) + ", " + tex_coord(src(sl, 1), unit) + ", " + tex_coord(src(sl, 2), unit) + ")"; break;
        case 0x2F: r = "textureLod(" + s + ", " + tex_coord(a, unit) + ", " + src(sl, 1) + ".x)"; break;
        case 0x31: r = "texture(" + s + ", " + tex_coord(a, unit) + ", " + src(sl, 1) + ".x)"; break;
        default: r = "vec4(0.0)"; break;
        }
        if ((sl.w[0] >> 21) & 1) r = "(" + r + " * 2.0 - 1.0)";
        return r;
    }

    std::string expr(const Slot& sl, uint32_t op) {
        auto a = [&] { return src(sl, 0); };
        auto b = [&] { return src(sl, 1); };
        auto c = [&] { return src(sl, 2); };
        switch (op) {
        case 0x01: return a();
        case 0x02: return a() + " * " + b();
        case 0x03: return a() + " + " + b();
        case 0x04: return "fma(" + a() + ", " + b() + ", " + c() + ")";
        case 0x05: return "vec4(dot(" + a() + ".xyz, " + b() + ".xyz))";
        case 0x06: return "vec4(dot(" + a() + ", " + b() + "))";
        case 0x07: return "vec4(1.0, " + a() + ".y * " + b() + ".y, " + a() + ".z, " + b() + ".w)";
        case 0x08: return "min(" + a() + ", " + b() + ")";
        case 0x09: return "max(" + a() + ", " + b() + ")";
        case 0x0A: return "vec4(lessThan(" + a() + ", " + b() + "))";
        case 0x0B: return "vec4(greaterThanEqual(" + a() + ", " + b() + "))";
        case 0x0C: return "vec4(lessThanEqual(" + a() + ", " + b() + "))";
        case 0x0D: return "vec4(greaterThan(" + a() + ", " + b() + "))";
        case 0x0E: return "vec4(notEqual(" + a() + ", " + b() + "))";
        case 0x0F: return "vec4(equal(" + a() + ", " + b() + "))";
        case 0x10: return "fract(" + a() + ")";
        case 0x11: return "floor(" + a() + ")";
        case 0x13: return "vec4(uintBitsToFloat(packSnorm4x8(" + a() + ")))";
        case 0x14: return "unpackSnorm4x8(floatBitsToUint(" + a() + ".x))";
        case 0x15: return "dFdx(" + a() + ")";
        case 0x16: return "dFdy(" + a() + ")";
        case 0x17: case 0x18: case 0x19: case 0x2F: case 0x31: return tex(sl, op);
        case 0x1A: return "vec4(1.0 / " + a() + ".x)";
        case 0x1B: return "vec4(inversesqrt(abs(" + a() + ".x)))";
        case 0x1C: return "vec4(exp2(" + a() + ".x))";
        case 0x1D: return "vec4(log2(" + a() + ".x))";
        case 0x1E: return "lit(" + a() + ")";
        case 0x1F: return "mix(" + c() + ", " + b() + ", " + a() + ")";
        case 0x20: return "vec4(1.0)";
        case 0x21: return "vec4(0.0)";
        case 0x22: return "vec4(cos(" + a() + ".x))";
        case 0x23: return "vec4(sin(" + a() + ".x))";
        case 0x24: return "vec4(uintBitsToFloat(packHalf2x16(" + a() + ".xy)))";
        case 0x25: return "unpackHalf2x16(floatBitsToUint(" + a() + ".x)).xyxy";
        case 0x26: return "exp2(" + b() + " * log2(" + a() + "))";
        case 0x27: return "vec4(uintBitsToFloat(packUnorm4x8(" + a() + ")))";
        case 0x28: return "unpackUnorm4x8(floatBitsToUint(" + a() + ".x))";
        case 0x29: return "vec4(uintBitsToFloat(packUnorm2x16(" + a() + ".xy)))";
        case 0x2A: return "unpackUnorm2x16(floatBitsToUint(" + a() + ".x)).xyxy";
        case 0x2B: return a() + ".xyxy + " + b() + ".xxxx * " + c() + ".xzxz + " + b() + ".yyyy * " + c() + ".ywyw";
        case 0x2C: return "vec4(uintBitsToFloat(packUnorm4x8(lin2srgb(" + a() + "))))";
        case 0x2D: return "srgb2lin(unpackUnorm4x8(floatBitsToUint(" + a() + ".x)))";
        case 0x2E: return "vec4(dot(" + a() + ".xy, " + b() + ".xy) + " + c() + ".x)";
        case 0x36: return "reflect(" + a() + ", " + b() + ")";
        case 0x38: return "vec4(dot(" + a() + ".xy, " + b() + ".xy))";
        case 0x39: return "nrm(" + a() + ")";
        case 0x3A: return a() + " / " + b() + ".x";
        case 0x3B: return "divsq(" + a() + ", " + b() + ".x)";
        case 0x3C: return "vec4(1.0, " + a() + ".y, " + a() + ".y > 0.0 ? exp2(" + a() + ".w) : 0.0, 1.0)";
        default: return "";
        }
    }

    std::string cc_test(const Slot& sl) {
        uint32_t s0 = sl.w[1];
        uint32_t bits = (s0 >> 18) & 7;
        uint32_t sel = (s0 >> 31) & 1;
        std::string sw = std::string(".") + lane(s0 >> 21) + lane(s0 >> 23) + lane(s0 >> 25) + lane(s0 >> 27);
        return "cctest(CC[" + std::to_string(sel) + "]" + sw + ", " + std::to_string(bits) + ")";
    }

    void emit_op(std::ostream& o, const Slot& sl, uint32_t op, const std::string& ind) {
        uint32_t bits = (sl.w[1] >> 18) & 7;
        if (bits == 0) return;
        bool cond = bits != 7;
        if (op == 0x12) {
            uses_kil_ = true;
            o << ind << (cond ? "if (any(" + cc_test(sl) + ")) " : "") << "discard;\n";
            return;
        }
        if (op == 0x3D || op == 0x3E) return;
        std::string e = expr(sl, op);
        if (e.empty()) {
            o << ind << "// unsupported opcode " << op << "\n";
            return;
        }
        uint32_t d = sl.w[0];
        uint32_t scale = (sl.w[2] >> 28) & 7;
        static const char* scales[8] = {"", " * 2.0", " * 4.0", " * 8.0", "", " * 0.5", " * 0.25", " * 0.125"};
        o << ind << "{\n";
        std::string in = ind + "    ";
        o << in << "vec4 t = (" << e << ")" << scales[scale] << ";\n";
        bool fp16 = (d >> 7) & 1;
        if (fp16) o << in << "t = clamp(t, -65504.0, 65504.0);\n";
        uint32_t prec = (d >> 22) & 3;
        if ((d >> 31) & 1) o << in << "t = sat(t);\n";
        else if (prec == 2) o << in << "t = clamp(t, -2.0, 2.0);\n";
        else if (prec == 3) o << in << "t = clamp(t, -1.0, 1.0);\n";
        if (cond) o << in << "bvec4 ct = " << cc_test(sl) << ";\n";
        uint32_t mask = (d >> 9) & 0xF;
        bool no_dest = (d >> 30) & 1;
        std::string dst = fp16 ? "H[" + std::to_string((d >> 1) & 0x3F) + "]" : "R[" + std::to_string((d >> 1) & 0x3F) + "]";
        bool set_cc = (d >> 8) & 1;
        std::string cc = "CC[" + std::to_string((sl.w[1] >> 30) & 1) + "]";
        for (uint32_t l = 0; l < 4; l++) {
            if (!(mask & (1u << l))) continue;
            std::string guard = cond ? std::string("if (ct.") + lane(l) + ") " : "";
            if (!no_dest) o << in << guard << dst << "." << lane(l) << " = t." << lane(l) << ";\n";
            if (set_cc) o << in << guard << cc << "." << lane(l) << " = t." << lane(l) << ";\n";
        }
        o << ind << "}\n";
    }

    void emit_flow(std::ostream& o, const Slot& sl, uint32_t op, uint32_t p, std::string& ind, std::vector<std::pair<uint32_t, std::string>>& closers) {
        uint32_t bits = (sl.w[1] >> 18) & 7;
        std::string test = bits == 7 ? "true" : bits == 0 ? "false" : "any(" + cc_test(sl) + ")";
        switch (op) {
        case 0x42: {
            uint32_t else_slot = (sl.w[2] & 0x7FFFFFFF) >> 2;
            uint32_t end_slot = sl.w[3] >> 2;
            o << ind << "if (" << test << ") {\n";
            ind += "    ";
            closers.push_back({end_slot, "}"});
            if (else_slot != end_slot) closers.push_back({else_slot, "} else {"});
            break;
        }
        case 0x43:
        case 0x44: {
            uint32_t count = (sl.w[2] >> 2) & 0xFF;
            int32_t init = (sl.w[2] >> 10) & 0xFF;
            int32_t inc = static_cast<int8_t>((sl.w[2] >> 19) & 0xFF);
            uint32_t end_slot = sl.w[3] >> 2;
            std::string lc = "lc" + std::to_string(p);
            o << ind << "if (" << test << ") for (int it" << p << " = 0, " << lc << " = " << init << "; it" << p << " < " << count << "; it" << p << "++, " << lc
              << " += " << inc << ") {\n";
            ind += "    ";
            closers.push_back({end_slot, "}"});
            break;
        }
        case 0x40: o << ind << "if (" << test << ") break;\n"; break;
        case 0x45: o << ind << "if (" << test << ") return;\n"; break;
        default: o << ind << "// unsupported flow opcode " << op << "\n"; break;
        }
    }

    std::string assemble(const std::string& body) {
        std::ostringstream s;
        s << "#version 450\n";
        s << "layout(location = 1) in vec4 in_col0;\nlayout(location = 2) in vec4 in_col1;\nlayout(location = 3) in vec4 in_fog;\n";
        for (uint32_t i = 0; i < 8; i++) s << "layout(location = " << (4 + i) << ") in vec4 in_tex" << i << ";\n";
        s << "layout(location = 12) in vec4 in_tex8;\nlayout(location = 13) in vec4 in_tex9;\nlayout(location = 14) in vec4 in_bfc0;\nlayout(location = 15) in vec4 in_bfc1;\n";
        uint32_t lits = static_cast<uint32_t>(l_.literal_slots.size());
        s << "layout(std140, set = 0, binding = 2) uniform FragmentConstants { vec4 fc[" << (lits ? lits : 1) << "]; };\n";
        s << "layout(std140, set = 0, binding = 3) uniform FragmentState { vec4 window; vec4 fog; uvec4 fflags; };\n";
        for (uint32_t u = 0; u < 16; u++) {
            if (!(used_tex_ & (1u << u))) continue;
            static const char* types[4] = {"sampler1D", "sampler2D", "samplerCube", "sampler3D"};
            s << "layout(set = 0, binding = " << (32 + u) << ") uniform " << types[dim(u)] << " tex" << u << ";\n";
        }
        uint32_t mrt = key_.mrt_count > 4 ? 4 : key_.mrt_count;
        for (uint32_t m = 0; m < (mrt ? mrt : 1); m++) s << "layout(location = " << m << ") out vec4 out_color" << m << ";\n";
        s << "vec4 R[48];\nvec4 H[96];\nvec4 CC[2];\n";
        s << "vec4 sat(vec4 v) { return clamp(mix(v, vec4(0.0), isnan(v)), 0.0, 1.0); }\n";
        s << "bvec4 cctest(vec4 v, uint b) {\n"
             "    bvec4 r = bvec4(false);\n"
             "    if ((b & 1u) != 0u) r = bvec4(uvec4(r) | uvec4(lessThan(v, vec4(0.0))));\n"
             "    if ((b & 2u) != 0u) r = bvec4(uvec4(r) | uvec4(equal(v, vec4(0.0))));\n"
             "    if ((b & 4u) != 0u) r = bvec4(uvec4(r) | uvec4(greaterThan(v, vec4(0.0))));\n"
             "    return r;\n}\n";
        s << "vec4 wpos() {\n"
             "    vec4 f = gl_FragCoord;\n"
             "    float y = window.y > 0.5 ? window.x - f.y : f.y;\n"
             "    vec2 p = vec2(f.x, y) - (window.z > 0.5 ? vec2(0.5) : vec2(0.0));\n"
             "    return vec4(p, f.z, f.w);\n}\n";
        s << "vec4 fogc() {\n"
             "    float c = in_fog.x;\n"
             "    uint mode = fflags.y;\n"
             "    float a = (mode >= 0x802u) ? abs(c) : c;\n"
             "    float y;\n"
             "    if (mode == 0x2601u || mode == 0x804u) y = fog.y * a + (fog.x - 1.0);\n"
             "    else if (mode == 0x800u || mode == 0x802u) y = exp(11.084 * (fog.y * a + fog.x - 1.5));\n"
             "    else { float k = 4.709 * (fog.y * a + fog.x - 1.5); y = exp(-(k * k)); }\n"
             "    return vec4(c, clamp(y, 0.0, 1.0), 0.0, 0.0);\n}\n";
        s << "vec4 lit(vec4 a) { float x = max(a.x, 0.0); return vec4(1.0, x, x > 0.0 ? pow(max(a.y, 1e-10), a.w) : 0.0, 1.0); }\n";
        s << "vec4 nrm(vec4 a) { float l = length(a.xyz); vec3 n = l > 0.0 ? a.xyz / l : a.xyz; return vec4(n, n.z); }\n";
        s << "vec4 divsq(vec4 a, float b) { vec4 r = a / sqrt(abs(b)); return mix(r, vec4(0.0), equal(a, vec4(0.0))); }\n";
        s << "vec4 lin2srgb(vec4 c) { vec3 x = clamp(c.rgb, 0.0, 1.0); vec3 r = mix(1.055 * pow(x, vec3(1.0 / 2.4)) - 0.055, 12.92 * x, lessThan(x, vec3(0.0031308))); return vec4(r, c.a); }\n";
        s << "vec4 srgb2lin(vec4 c) { vec3 x = c.rgb; vec3 r = mix(pow((x + 0.055) / 1.055, vec3(2.4)), x / 12.92, lessThanEqual(x, vec3(0.04045))); return vec4(r, c.a); }\n";
        s << "void program() {\n" << body << "}\n";
        s << "void main() {\n";
        s << "    for (int i = 0; i < 48; i++) R[i] = vec4(0.0);\n    for (int i = 0; i < 96; i++) H[i] = vec4(0.0);\n    CC[0] = vec4(0.0); CC[1] = vec4(0.0);\n";
        s << "    program();\n";
        bool full = key_.control & 0x40;
        const char* r_out[4] = {"R[0]", "R[2]", "R[3]", "R[4]"};
        const char* h_out[4] = {"H[0]", "H[4]", "H[6]", "H[8]"};
        s << "    vec4 c0 = " << (full ? r_out[0] : h_out[0]) << ";\n";
        s << "    if ((fflags.x & 1u) != 0u) {\n"
             "        uint f = fflags.z & 7u; float a = c0.a; float r = window.w; bool pass = true;\n"
             "        if (f == 0u) pass = false; else if (f == 1u) pass = a < r; else if (f == 2u) pass = a == r; else if (f == 3u) pass = a <= r;\n"
             "        else if (f == 4u) pass = a > r; else if (f == 5u) pass = a != r; else if (f == 6u) pass = a >= r;\n"
             "        if (!pass) discard;\n    }\n";
        for (uint32_t m = 0; m < (mrt ? mrt : 1); m++) {
            std::string v = m == 0 ? "c0" : (full ? r_out[m] : h_out[m]);
            s << "    out_color" << m << " = ((fflags.x & 2u) != 0u && " << (full ? "false" : "true") << ") ? lin2srgb(" << v << ") : " << v << ";\n";
        }
        s << "    if ((fflags.w & 1u) != 0u) out_color0 = vec4(1.0, 0.0, 1.0, 1.0);\n";
        s << "    if ((fflags.w & 2u) != 0u) out_color0 = vec4(c0.rgb, 1.0);\n";
        if (key_.control & 0xE) s << "    gl_FragDepth = R[1].z;\n";
        s << "}\n";
        return s.str();
    }
};

}

FragmentProgramInfo analyse_fragment_program(const FpWordReader& read) {
    Layout l = layout(read);
    FragmentProgramInfo info;
    info.valid = l.valid;
    info.literal_slots = l.literal_slots;
    info.texture_mask = l.texture_mask;
    info.length_slots = l.length;
    info.start_slot = l.start;
    info.hash = l.hash;
    return info;
}

FragmentProgramInfo decompile_fragment_program(const FpWordReader& read, const FragmentProgramKey& key) {
    Layout l = layout(read);
    FragmentProgramInfo info;
    if (!l.valid) return info;
    Decompiler d(read, key, l);
    info.glsl = d.run();
    info.literal_slots = l.literal_slots;
    info.texture_mask = d.texture_mask();
    info.length_slots = l.length;
    info.start_slot = l.start;
    info.hash = l.hash;
    info.valid = true;
    return info;
}

}
