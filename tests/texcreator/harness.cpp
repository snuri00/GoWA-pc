#include "texcreator_native.h"

#include "spu/context.h"
#include "spu_decode.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <algorithm>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <xmmintrin.h>

#include <windows.h>

namespace {

constexpr uint32_t kImageEa = 0x730800;
constexpr uint32_t kImageFileOffset = 0x720800;
constexpr uint32_t kImageSize = 0x4ea0;
constexpr uint32_t kImageLs = 0x4000;
constexpr uint32_t kComputeEntry = 0x56e0;
constexpr uint32_t kParamLs = 0x8f00;
constexpr uint32_t kOutLs = 0x9040;
constexpr uint32_t kReturnPc = 0x0;
constexpr uint32_t kStack = 0x3f890;

class NullHost : public spu::Host {
public:
    uint32_t read_channel(uint32_t) override { return 0; }
    void write_channel(uint32_t, uint32_t) override {}
    uint32_t channel_count(uint32_t) override { return 0; }
    bool stop_and_signal(uint32_t) override { return false; }
    void halt(uint32_t, uint32_t) override {}
};

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
void put_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

std::vector<uint8_t> hex_bytes(const std::string& s) {
    std::vector<uint8_t> out;
    int hi = -1;
    for (char ch : s) {
        int v;
        if (ch >= '0' && ch <= '9') v = ch - '0';
        else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
        else continue;
        if (hi < 0) hi = v;
        else {
            out.push_back(uint8_t(hi << 4 | v));
            hi = -1;
        }
    }
    return out;
}

struct Put {
    uint32_t lsa = 0;
    uint32_t ea = 0;
    std::vector<uint8_t> data;
};

struct DatasetJob {
    std::vector<uint8_t> args;
    std::vector<uint8_t> param_in;
    std::vector<uint8_t> param_out;
    std::vector<Put> puts;
};

std::vector<DatasetJob> load_dataset(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    std::vector<std::vector<uint8_t>> args_queue;
    std::vector<DatasetJob> jobs;
    bool pending_args = false;
    bool pending_param = false;
    bool in_job = false;
    while (std::getline(f, line)) {
        std::istringstream is(line);
        std::string kind;
        is >> kind;
        if (kind == "GET") {
            std::string k;
            uint32_t lsa = 0, ea = 0, size = 0, pc = 0;
            while (is >> k) {
                std::string v;
                is >> v;
                if (k == "lsa") lsa = std::stoul(v, nullptr, 16);
                else if (k == "ea") ea = std::stoul(v, nullptr, 16);
                else if (k == "size") size = std::stoul(v, nullptr, 16);
                else if (k == "pc") pc = std::stoul(v, nullptr, 16);
            }
            pending_args = lsa == 0xc00 && size == texcreator::kArgsSize;
            pending_param = pc == 0x083c0 && lsa == kParamLs && size == texcreator::kParamSize;
            if (!pending_param && in_job && !(pc == 0x083c0)) in_job = false;
        } else if (kind == "GETDATA") {
            size_t p = line.find("data ");
            std::vector<uint8_t> d = hex_bytes(line.substr(p + 5));
            if (pending_args && d.size() == texcreator::kArgsSize && be32(d.data() + 0x0c) == kImageEa) args_queue.push_back(d);
            if (pending_param) {
                DatasetJob j;
                j.param_in = d;
                jobs.push_back(j);
                in_job = true;
            }
            pending_args = pending_param = false;
        } else if (kind == "PUT") {
            std::string k;
            Put put;
            uint32_t pc = 0;
            std::string data;
            while (is >> k) {
                if (k == "data") {
                    std::string rest;
                    std::getline(is, rest);
                    data = rest;
                    break;
                }
                std::string v;
                is >> v;
                if (k == "lsa") put.lsa = std::stoul(v, nullptr, 16);
                else if (k == "ea") put.ea = std::stoul(v, nullptr, 16);
                else if (k == "pc") pc = std::stoul(v, nullptr, 16);
            }
            if (in_job && pc == 0x083c0) {
                put.data = hex_bytes(data);
                if (put.lsa == kParamLs) jobs.back().param_out = put.data;
                else jobs.back().puts.push_back(put);
            } else in_job = false;
        }
    }
    for (auto& j : jobs) {
        if (j.puts.empty()) continue;
        for (auto& a : args_queue) {
            if (be32(a.data() + 0x20) == j.puts[0].ea) j.args = a;
        }
    }
    return jobs;
}

std::vector<uint8_t> g_image;

struct SpuRun {
    std::vector<uint8_t> ls;
    uint64_t insns = 0;
    bool ok = false;
};

using Probe = void (*)(const spu::Context&, void*);

struct TraceRec {
    uint32_t pc;
    uint32_t w;
    uint32_t mem;
    int32_t dep[4];
    int32_t memdep;
    spu::v128 val;
};

std::vector<TraceRec>* g_trace = nullptr;

void set_reg(spu::Context& c, int r, uint32_t a, uint32_t b, uint32_t d, uint32_t e) {
    c.gpr[r].u32[3] = a;
    c.gpr[r].u32[2] = b;
    c.gpr[r].u32[1] = d;
    c.gpr[r].u32[0] = e;
}

SpuRun run_spu(const uint8_t* param_be, const texcreator::Job& job, const std::vector<uint32_t>& probes = {}, Probe probe = nullptr, void* user = nullptr) {
    SpuRun r;
    r.ls.assign(spu::LS_SIZE, 0);
    std::memcpy(r.ls.data() + kImageLs, g_image.data(), kImageSize);
    std::memcpy(r.ls.data() + kParamLs, param_be, texcreator::kParamSize);
    spu::Context c{};
    c.ls = r.ls.data();
    uint32_t outsz = texcreator::output_size(job);
    set_reg(c, 0, kReturnPc, 0, 0, 0);
    set_reg(c, 1, kStack, 0x18d0, 0xffffff50, 0xffffff50);
    set_reg(c, 3, kParamLs, kOutLs, job.width, job.height);
    set_reg(c, 4, kOutLs, job.width, job.height, kParamLs);
    set_reg(c, 5, job.width, job.height, kParamLs, kOutLs);
    set_reg(c, 6, job.height, kParamLs, kOutLs, job.width);
    set_reg(c, 7, job.depth, job.rows, job.row_start, 0xffffffa0);
    set_reg(c, 8, job.rows, job.row_start, 0xffffffa0, job.depth);
    set_reg(c, 9, job.row_start, 0xffffffa0, job.depth, job.rows);
    set_reg(c, 10, kOutLs + outsz, 0x64d0, 0, 0);
    c.pc = kComputeEntry;
    NullHost host;
    std::set<uint32_t> pset(probes.begin(), probes.end());
    std::vector<int32_t> last_def(128, -1);
    std::vector<int32_t> last_store(spu::LS_SIZE / 16, -1);
    while (r.insns < 2000000000ull) {
        if (c.pc == kReturnPc) {
            r.ok = true;
            break;
        }
        if (probe && pset.count(c.pc)) probe(c, user);
        if (g_trace) {
            uint32_t pc = c.pc;
            uint32_t w = be32(c.ls + pc);
            spudec::Insn in = spudec::decode(w);
            TraceRec t{};
            t.pc = pc;
            t.w = w;
            t.mem = ~0u;
            t.memdep = -1;
            for (int k = 0; k < 4; k++) t.dep[k] = k < in.nsrc ? last_def[in.src[k]] : -1;
            if (in.load || in.store) {
                uint32_t a = 0;
                if (in.pc_rel) a = pc + in.imm * 4;
                else if (in.abs_addr) a = in.imm * 4;
                else {
                    a = c.gpr[in.addr_a].u32[3] + (in.addr_b >= 0 ? c.gpr[in.addr_b].u32[3] : uint32_t(in.imm));
                }
                t.mem = a & spu::LS_MASK;
                if (in.load) t.memdep = last_store[t.mem / 16];
            }
            if (!spu::step(c, host)) break;
            int32_t idx = int32_t(g_trace->size());
            if (in.dst >= 0) {
                t.val = c.gpr[in.dst];
                last_def[in.dst] = idx;
            }
            if (in.store) {
                t.val = c.gpr[in.src[0]];
                last_store[t.mem / 16] = idx;
            }
            g_trace->push_back(t);
            r.insns++;
            continue;
        }
        if (!spu::step(c, host)) break;
        r.insns++;
    }
    return r;
}

std::vector<uint8_t> spu_output(const SpuRun& r, const texcreator::Job& job) {
    uint32_t n = texcreator::output_size(job);
    return std::vector<uint8_t>(r.ls.begin() + kOutLs, r.ls.begin() + kOutLs + n);
}

std::vector<uint8_t> spu_param(const SpuRun& r) { return std::vector<uint8_t>(r.ls.begin() + kParamLs, r.ls.begin() + kParamLs + texcreator::kParamSize); }

float half_to_float(uint16_t h) {
    uint32_t s = uint32_t(h >> 15) << 31;
    uint32_t e = (h >> 10) & 31;
    uint32_t m = h & 1023;
    if (e == 0) {
        float v = std::ldexp(float(m), -24);
        return s ? -v : v;
    }
    uint32_t bits = s | ((e - 15 + 127) << 23) | (m << 13);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

struct Diff {
    size_t bytes_diff = 0;
    size_t halfs = 0;
    size_t halfs_diff = 0;
    int max_ulp = 0;
    size_t first = SIZE_MAX;
};

Diff compare_halfs(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    Diff d;
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint16_t x = uint16_t(a[i] << 8 | a[i + 1]);
        uint16_t y = uint16_t(b[i] << 8 | b[i + 1]);
        d.halfs++;
        if (x != y) {
            d.halfs_diff++;
            d.bytes_diff += 2;
            if (d.first == SIZE_MAX) d.first = i;
            int ix = (x & 0x8000) ? -(x & 0x7fff) : x;
            int iy = (y & 0x8000) ? -(y & 0x7fff) : y;
            d.max_ulp = std::max(d.max_ulp, std::abs(ix - iy));
        }
    }
    if (a.size() != b.size()) d.bytes_diff += 1;
    return d;
}

std::vector<uint8_t> captured_output(const DatasetJob& j, const texcreator::Job& job) {
    uint32_t slice = job.rows * job.width * 8;
    std::vector<uint8_t> out(texcreator::output_size(job));
    for (size_t k = 0; k < j.puts.size() && k < job.depth; k++) std::memcpy(out.data() + k * slice, j.puts[k].data.data(), std::min<size_t>(slice, j.puts[k].data.size()));
    return out;
}

void print_diff(const char* what, const Diff& d) {
    std::printf("  %-14s halfs %zu differing %zu max_ulp %d", what, d.halfs, d.halfs_diff, d.max_ulp);
    if (d.first != SIZE_MAX) std::printf(" first_byte 0x%zx", d.first);
    std::printf("\n");
}

struct TraceCtx {
    int lo = 0;
    int hi = 127;
    uint32_t max_hits = 4;
    std::map<uint32_t, uint32_t>* hits;
};

void trace_probe(const spu::Context& c, void* user) {
    auto* t = static_cast<TraceCtx*>(user);
    if (++(*t->hits)[c.pc] > t->max_hits) return;
    std::printf("pc %05x\n", c.pc);
    for (int r = t->lo; r <= t->hi; r++) {
        const spu::v128& v = c.gpr[r];
        float f[4];
        std::memcpy(f, v.u32, 16);
        std::printf("  r%-3d %08x %08x %08x %08x | %.9g %.9g %.9g %.9g\n", r, v.u32[3], v.u32[2], v.u32[1], v.u32[0], f[3], f[2], f[1], f[0]);
    }
}


int cmd_dataset(const std::vector<DatasetJob>& jobs) {
    int fails = 0;
    for (size_t i = 0; i < jobs.size(); i++) {
        const DatasetJob& j = jobs[i];
        if (j.args.empty()) {
            std::printf("job %zu: no args\n", i);
            fails++;
            continue;
        }
        texcreator::Job job = texcreator::parse_args(j.args.data());
        std::printf("job %zu: out_ea %08x param_ea %08x %ux%ux%u rows %u start %u puts %zu\n", i, job.out_ea, job.param_ea, job.width, job.height, job.depth, job.rows, job.row_start, j.puts.size());
        std::vector<uint8_t> want = captured_output(j, job);
        SpuRun r = run_spu(j.param_in.data(), job);
        std::vector<uint8_t> got = spu_output(r, job);
        Diff ds = compare_halfs(got, want);
        Diff dps = compare_halfs(spu_param(r), j.param_out);
        print_diff("spu-vs-capture", ds);
        print_diff("spu-param", dps);
        std::vector<uint8_t> param = j.param_in;
        std::vector<uint8_t> nat(texcreator::output_size(job));
        texcreator::run_job(param.data(), job, nat.data());
        Diff dn = compare_halfs(nat, want);
        Diff dpn = compare_halfs(param, j.param_out);
        print_diff("native-vs-cap", dn);
        print_diff("native-param", dpn);
        if (ds.halfs_diff || dn.halfs_diff || dps.halfs_diff || dpn.halfs_diff || !r.ok) fails++;
    }
    std::printf("dataset: %zu jobs, %d failing\n", jobs.size(), fails);
    return fails;
}

float urand(std::mt19937& g, float lo, float hi) { return lo + (hi - lo) * std::uniform_real_distribution<float>(0.0f, 1.0f)(g); }

void put_f(std::vector<uint8_t>& p, uint32_t off, float v) {
    uint32_t b;
    std::memcpy(&b, &v, 4);
    put_be32(p.data() + off, b);
}

struct SynthCase {
    std::vector<uint8_t> param;
    texcreator::Job job;
};

SynthCase random_case(std::mt19937& g, int mode) {
    SynthCase c;
    c.param.assign(texcreator::kParamSize, 0);
    auto& p = c.param;
    float qw = urand(g, -1, 1), qx = urand(g, -1, 1), qy = urand(g, -1, 1), qz = urand(g, -1, 1);
    float n = std::sqrt(qw * qw + qx * qx + qy * qy + qz * qz);
    if (n < 1e-3f) qw = n = 1.0f;
    qw /= n, qx /= n, qy /= n, qz /= n;
    float m[3][3] = {{1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw)},
                     {2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw)},
                     {2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)}};
    if (mode == 2) {
        int pick = g() % 3;
        float s = (g() & 1) ? 1.0f : -1.0f;
        float up[3][3] = {{1, 0, 0}, {0, 0, s}, {0, -s, 0}};
        if (pick == 1) up[2][1] = -s * 1.0001f;
        if (pick == 2) up[2][1] = -s * 0.9999f;
        std::memcpy(m, up, sizeof m);
    }
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) put_f(p, r * 16 + k * 4, m[r][k]);
    float range = mode == 1 ? 2000.0f : 100.0f;
    put_f(p, 0x30, urand(g, -range, range));
    put_f(p, 0x34, urand(g, -range, range));
    put_f(p, 0x38, urand(g, -range, range));
    put_f(p, 0x3c, 1.0f);
    put_f(p, 0x40, urand(g, 0.05f, 1.3f));
    put_f(p, 0x44, urand(g, 0.05f, 1.5f));
    put_f(p, 0x48, urand(g, -1, 1));
    put_f(p, 0x4c, urand(g, -1, 1));
    for (int k = 0; k < 3; k++) put_f(p, 0x50 + k * 4, urand(g, -30, 30));
    for (int k = 0; k < 4; k++) put_f(p, 0x60 + k * 4, urand(g, -5, 5));
    put_f(p, 0x70, urand(g, mode == 1 ? -50.0f : 0.0f, mode == 1 ? 50.0f : 8.0f));
    put_f(p, 0x74, urand(g, mode == 1 ? -5.0f : 0.01f, mode == 1 ? 5.0f : 1.5f));
    put_f(p, 0x78, urand(g, mode == 1 ? -2.0f : 0.005f, mode == 1 ? 2.0f : 0.6f));
    put_f(p, 0x7c, urand(g, -0.2f, 1.0f));
    put_f(p, 0x80, urand(g, -0.2f, 1.2f));
    put_f(p, 0x84, urand(g, -0.2f, 1.0f));
    put_f(p, 0x88, urand(g, -0.2f, 1.2f));
    put_f(p, 0x8c, urand(g, mode == 1 ? -4.0f : 0.1f, 4.0f));
    for (int k = 0; k < 16; k++) put_f(p, 0x90 + k * 4, urand(g, -600, 600));
    for (int k = 0; k < 4; k++) put_f(p, 0xd0 + k * 4, urand(g, -2, 2));
    for (int k = 0; k < 4; k++) put_f(p, 0xe0 + k * 4, urand(g, -0.1f, 1.1f));
    if (g() % 4 == 0) put_f(p, 0xe4, urand(g, -0.1f, 1.1f) * 0.5f);
    for (int col = 0; col < 3; col++)
        for (int k = 0; k < 4; k++) put_f(p, 0xf0 + col * 16 + k * 4, urand(g, -0.1f, 1.3f));
    for (int k = 0; k < 8; k++) put_f(p, 0x120 + k * 4, urand(g, 0.3f, 1.3f));
    static const uint32_t widths[] = {64, 64, 64, 32, 16, 8};
    static const uint32_t heights[] = {32, 32, 32, 16, 64, 8};
    static const uint32_t depths[] = {16, 16, 16, 8, 4, 2};
    static const uint32_t rows[] = {2, 2, 2, 4, 1, 8};
    if (mode == 4) {
        int n = 1 + int(g() % 3);
        for (int i = 0; i < n; i++) {
            float sgn = (g() & 1) ? 1.0f : -1.0f;
            switch (g() % 10) {
            case 0: put_f(p, 0x78, sgn * 1e36f); break;
            case 1: put_f(p, 0x30, sgn * 3e38f); break;
            case 2: put_f(p, 0x74, 1e38f); break;
            case 3: put_f(p, 0x44, 0.0f); break;
            case 4: put_f(p, 0x40, 0.0f); break;
            case 5: put_f(p, 0x8c, 3e38f); break;
            case 6: put_f(p, 0x100, 3e38f); break;
            case 7: put_be32(p.data() + 0x70, 0x7fffffffu); break;
            case 8: put_f(p, 0xe4, urand(g, 0, 1)); std::memcpy(p.data() + 0xe0, p.data() + 0xe4, 4); std::memcpy(p.data() + 0xe8, p.data() + 0xe4, 4); break;
            case 9: put_f(p, 0x38, sgn * 1e30f); put_f(p, 0x78, 1e8f); break;
            }
        }
    }
    int geo = mode == 3 ? int(g() % 6) : 0;
    c.job.width = widths[geo];
    c.job.height = heights[geo];
    c.job.depth = depths[geo];
    c.job.rows = rows[geo];
    c.job.row_start = (g() % (c.job.height / c.job.rows)) * c.job.rows;
    return c;
}

int cmd_synth(const std::vector<std::string>& args) {
    int count = args.size() > 1 ? std::stoi(args[1]) : 100;
    uint32_t seed = args.size() > 2 ? std::stoul(args[2]) : 1;
    std::mt19937 g(seed);
    int fails[5] = {}, runs[5] = {}, worst = 0;
    int modes = args.size() > 3 ? std::stoi(args[3]) : 4;
    size_t halfs = 0, halfs_bad = 0;
    for (int i = 0; i < count; i++) {
        int mode = i % modes;
        SynthCase c = random_case(g, mode);
        SpuRun r = run_spu(c.param.data(), c.job);
        std::vector<uint8_t> want = spu_output(r, c.job);
        std::vector<uint8_t> param = c.param;
        std::vector<uint8_t> got(texcreator::output_size(c.job));
        texcreator::run_job(param.data(), c.job, got.data());
        Diff d = compare_halfs(got, want);
        Diff dp = compare_halfs(param, spu_param(r));
        runs[mode]++;
        if (mode < 4) {
            halfs += d.halfs;
            halfs_bad += d.halfs_diff;
            worst = std::max(worst, d.max_ulp);
        }
        if (d.halfs_diff || dp.halfs_diff || !r.ok) {
            fails[mode]++;
            if (fails[mode] <= 3) {
                std::printf("case %d mode %d %ux%ux%u rows %u start %u: out diff %zu/%zu max_ulp %d first 0x%zx, param diff %zu ok %d\n", i, mode, c.job.width, c.job.height, c.job.depth, c.job.rows, c.job.row_start,
                            d.halfs_diff, d.halfs, d.max_ulp, d.first, dp.halfs_diff, r.ok);
                if (d.first != SIZE_MAX) {
                    size_t o = d.first & ~size_t(7);
                    std::printf("   spu    %02x%02x %02x%02x %02x%02x %02x%02x\n   native %02x%02x %02x%02x %02x%02x %02x%02x\n", want[o], want[o + 1], want[o + 2], want[o + 3], want[o + 4], want[o + 5], want[o + 6], want[o + 7], got[o],
                                got[o + 1], got[o + 2], got[o + 3], got[o + 4], got[o + 5], got[o + 6], got[o + 7]);
                }
            }
        }
    }
    int total_fail = fails[0] + fails[1] + fails[2] + fails[3] + fails[4];
    std::printf("synthetic: %d cases, %d pass, %d fail (typical %d/%d, wide %d/%d, axis-aligned %d/%d, geometry %d/%d, extreme %d/%d failing); regular-mode halfs %zu differing %zu max_ulp %d\n", count, count - total_fail, total_fail, fails[0], runs[0],
                fails[1], runs[1], fails[2], runs[2], fails[3], runs[3], fails[4], runs[4], halfs, halfs_bad, worst);
    return total_fail ? 1 : 0;
}

void table_probe(const spu::Context& c, void* user) {
    uint32_t h = *static_cast<uint32_t*>(user);
    if (c.pc == 0x5808) {
        for (uint32_t r = 0; r < h; r++) {
            float f[4];
            for (int k = 0; k < 4; k++) {
                uint32_t b = be32(c.ls + 0x3ea30 + r * 16 + k * 4);
                std::memcpy(&f[k], &b, 4);
            }
            std::printf("  T[%2u] %.9g %.9g %.9g %.9g\n", r, f[0], f[1], f[2], f[3]);
        }
        return;
    }
    float f[4];
    std::memcpy(f, &c.gpr[14].u32[3], 4);
    std::memcpy(f + 1, &c.gpr[17].u32[3], 4);
    std::memcpy(f + 2, &c.gpr[8].u32[3], 4);
    std::printf("  pc %05x lo %.9g hi %.9g k0 %.9g r16 %08x r24 %08x\n", c.pc, f[0], f[1], f[2], c.gpr[16].u32[3], c.gpr[24].u32[3]);
    for (int k = 0; k < 8; k++) std::printf("   f[%d] %08x row %08x\n", k, be32(c.ls + c.gpr[1].u32[3] + 32 + k * 4), be32(c.ls + c.gpr[1].u32[3] + 64 + k * 4));
}

int cmd_synthdbg(const std::vector<std::string>& args) {
    int idx = std::stoi(args.at(1));
    uint32_t seed = args.size() > 2 ? std::stoul(args[2]) : 1;
    std::mt19937 g(seed);
    SynthCase c;
    for (int i = 0; i <= idx; i++) c = random_case(g, i % 4);
    for (int k = 0; k < 80; k++) {
        uint32_t b = be32(c.param.data() + k * 4);
        float f;
        std::memcpy(&f, &b, 4);
        std::printf("%s%.9g", k % 4 ? " " : (k ? "\n" : ""), f);
    }
    std::printf("\n");
    uint32_t h = c.job.height;
    run_spu(c.param.data(), c.job, {0x4220, 0x4294, 0x5808}, table_probe, &h);
    return 0;
}

int cmd_noise(const std::vector<std::string>& args) {
    int count = args.size() > 1 ? std::stoi(args[1]) : 10000;
    uint32_t seed = args.size() > 2 ? std::stoul(args[2]) : 7;
    std::mt19937 g(seed);
    std::vector<uint8_t> ls(spu::LS_SIZE, 0);
    std::memcpy(ls.data() + kImageLs, g_image.data(), kImageSize);
    NullHost host;
    int bad = 0;
    for (int i = 0; i < count; i++) {
        float scale = std::pow(10.0f, urand(g, -3.0f, 3.0f));
        spu::Context c{};
        c.ls = ls.data();
        float pts[4][4];
        for (int k = 0; k < 4; k++) {
            for (int j = 0; j < 4; j++) pts[k][j] = urand(g, -1, 1) * scale;
            if (i % 5 == 0) pts[k][g() % 3] = float(int(pts[k][0]));
            uint32_t w[4];
            std::memcpy(w, pts[k], 16);
            set_reg(c, 3 + k, w[0], w[1], w[2], w[3]);
        }
        set_reg(c, 0, kReturnPc, 0, 0, 0);
        set_reg(c, 1, kStack, 0, 0, 0);
        c.pc = 0x4930;
        uint64_t n = 0;
        while (c.pc != kReturnPc && n++ < 100000)
            if (!spu::step(c, host)) break;
        for (int k = 0; k < 4; k++) {
            float want;
            std::memcpy(&want, &c.gpr[3].u32[3 - k], 4);
            float got = texcreator::perlin_noise(pts[k][0], pts[k][1], pts[k][2]);
            if (std::memcmp(&want, &got, 4) != 0) {
                if (bad < 10) std::printf("noise mismatch (%.9g %.9g %.9g): spu %.9g native %.9g\n", pts[k][0], pts[k][1], pts[k][2], want, got);
                bad++;
            }
        }
    }
    std::printf("noise: %d points, %d mismatching\n", count * 4, bad);
    return bad ? 1 : 0;
}

}

std::map<uint32_t, uint32_t> g_hits;

void print_rec(const std::vector<TraceRec>& tr, int32_t i) {
    const TraceRec& t = tr[i];
    std::string d = spudec::disasm(t.w, t.pc);
    spudec::Insn in = spudec::decode(t.w);
    std::printf("#%-8d %05x %-28s", i, t.pc, d.c_str());
    std::printf(" [");
    for (int k = 0; k < in.nsrc; k++) std::printf("%s%d", k ? "," : "", t.dep[k]);
    if (t.mem != ~0u) std::printf(" m%05x:%d", t.mem, t.memdep);
    std::printf("]");
    if (in.dst >= 0 || in.store) {
        float f[4];
        std::memcpy(f, t.val.u32, 16);
        std::printf(" = %08x %08x %08x %08x | %.8g %.8g %.8g %.8g", t.val.u32[3], t.val.u32[2], t.val.u32[1], t.val.u32[0], f[3], f[2], f[1], f[0]);
    }
    std::printf("\n");
}

int cmd_slice(const std::vector<DatasetJob>& jobs, const std::vector<std::string>& args) {
    size_t ji = std::stoul(args.at(1));
    uint32_t addr = std::stoul(args.at(2), nullptr, 16) & spu::LS_MASK;
    std::set<uint32_t> stop_pcs;
    if (args.size() > 3) {
        std::stringstream ss(args[3]);
        std::string t;
        while (std::getline(ss, t, ',')) stop_pcs.insert(std::stoul(t, nullptr, 16));
    }
    std::vector<TraceRec> tr;
    tr.reserve(2500000);
    g_trace = &tr;
    texcreator::Job job = texcreator::parse_args(jobs.at(ji).args.data());
    run_spu(jobs[ji].param_in.data(), job);
    g_trace = nullptr;
    int32_t root = -1;
    if (const char* rr = std::getenv("TC_ROOT")) {
        uint32_t pc = 0, hit = 0;
        int reg = 0;
        std::sscanf(rr, "%x:%u:%d", &pc, &hit, &reg);
        uint32_t seen = 0;
        int32_t at = -1;
        for (int32_t i = 0; i < int32_t(tr.size()); i++)
            if (tr[i].pc == pc && seen++ == hit) {
                at = i;
                break;
            }
        for (int32_t i = at - 1; i >= 0 && root < 0; i--) {
            spudec::Insn in = spudec::decode(tr[i].w);
            if (in.dst == reg) root = i;
        }
    } else {
        for (int32_t i = int32_t(tr.size()) - 1; i >= 0; i--) {
            spudec::Insn in = spudec::decode(tr[i].w);
            if (in.store && tr[i].mem == addr) {
                root = i;
                break;
            }
        }
    }
    if (root < 0) {
        std::printf("no store to %05x\n", addr);
        return 1;
    }
    std::vector<uint8_t> mark(tr.size(), 0);
    std::vector<int32_t> work{root};
    mark[root] = 1;
    while (!work.empty()) {
        int32_t i = work.back();
        work.pop_back();
        if (stop_pcs.count(tr[i].pc) && i != root) continue;
        auto push = [&](int32_t d) {
            if (d >= 0 && !mark[d]) {
                mark[d] = 1;
                work.push_back(d);
            }
        };
        for (int k = 0; k < 4; k++) push(tr[i].dep[k]);
        push(tr[i].memdep);
    }
    size_t n = 0;
    for (int32_t i = 0; i < int32_t(tr.size()); i++)
        if (mark[i]) {
            print_rec(tr, i);
            n++;
        }
    std::printf("slice %zu of %zu\n", n, tr.size());
    return 0;
}

int main(int argc, char** argv) {
    _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x6000u | 0x8040u);
    std::string root = GOWA_ROOT;
    std::string elf = root + "/bin/01.12/GOWA.patched.elf";
    std::string dataset = root + "/out/texcreator/cap5_dma.txt";
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string cmd = args.empty() ? "all" : args[0];
    std::vector<uint8_t> e = read_file(elf);
    if (e.size() < kImageFileOffset + kImageSize) {
        std::fprintf(stderr, "cannot read %s\n", elf.c_str());
        return 2;
    }
    g_image.assign(e.begin() + kImageFileOffset, e.begin() + kImageFileOffset + kImageSize);
    std::vector<DatasetJob> jobs = load_dataset(dataset);
    if (const char* set = std::getenv("TC_SET")) {
        std::stringstream ss(set);
        std::string t;
        while (std::getline(ss, t, ',')) {
            size_t eq = t.find('=');
            uint32_t off = std::stoul(t.substr(0, eq), nullptr, 16);
            float v = std::stof(t.substr(eq + 1));
            uint32_t bits;
            std::memcpy(&bits, &v, 4);
            for (auto& j : jobs) put_be32(j.param_in.data() + off, bits);
        }
    }
    if (cmd == "dataset") return cmd_dataset(jobs) ? 1 : 0;
    if (cmd == "slice") return cmd_slice(jobs, args);
    if (cmd == "synth") return cmd_synth(args);
    if (cmd == "noise") return cmd_noise(args);
    if (cmd == "synthdbg") return cmd_synthdbg(args);
    if (cmd == "coverage") {
        int count = args.size() > 1 ? std::stoi(args[1]) : 100;
        std::mt19937 g(args.size() > 2 ? std::stoul(args[2]) : 1);
        std::vector<uint32_t> pcs = {0x4064, 0x4674, 0x4638, 0x4200, 0x44a0, 0x4778, 0x44d0, 0x48e8, 0x4508, 0x43d4, 0x4470, 0x46d8, 0x4870, 0x4488, 0x4680, 0x43f0, 0x47c0, 0x4578, 0x4548};
        static std::map<uint32_t, uint32_t> hits;
        hits.clear();
        Probe pr = [](const spu::Context& c, void*) { hits[c.pc]++; };
        for (int i = 0; i < count; i++) {
            SynthCase c = random_case(g, i % 5);
            texcreator::Job j = c.job;
            j.depth = 1;
            run_spu(c.param.data(), j, pcs, pr, nullptr);
        }
        for (uint32_t pc : pcs) std::printf("%05x %u\n", pc, hits[pc]);
        return 0;
    }
    if (cmd == "volume") {
        int count = args.size() > 1 ? std::stoi(args[1]) : 2;
        std::mt19937 g(args.size() > 2 ? std::stoul(args[2]) : 9);
        int bad = 0;
        for (int i = 0; i < count; i++) {
            SynthCase c = random_case(g, i % 2);
            const texcreator::Job& j = c.job;
            std::vector<uint8_t> want(size_t(j.width) * j.height * j.depth * 8), got(want.size());
            std::vector<uint8_t> pspu = c.param;
            for (uint32_t r0 = 0; r0 < j.height; r0 += j.rows) {
                texcreator::Job jj = j;
                jj.row_start = r0;
                SpuRun r = run_spu(pspu.data(), jj);
                std::vector<uint8_t> o = spu_output(r, jj);
                pspu = spu_param(r);
                for (uint32_t z = 0; z < j.depth; z++)
                    std::memcpy(want.data() + (size_t(z) * j.height + r0) * j.width * 8, o.data() + size_t(z) * j.rows * j.width * 8, size_t(j.rows) * j.width * 8);
            }
            std::vector<uint8_t> pn = c.param;
            texcreator::run_volume(pn.data(), j.width, j.height, j.depth, j.rows, got.data());
            Diff d = compare_halfs(got, want);
            Diff dp = compare_halfs(pn, pspu);
            std::printf("volume %d: %ux%ux%u in %u-row jobs: halfs %zu differing %zu, param differing %zu\n", i, j.width, j.height, j.depth, j.rows, d.halfs, d.halfs_diff, dp.halfs_diff);
            if (d.halfs_diff || dp.halfs_diff) bad++;
        }
        return bad ? 1 : 0;
    }
    if (cmd == "bench") {
        int reps = args.size() > 1 ? std::stoi(args[1]) : 50;
        std::mt19937 g(5);
        SynthCase c = random_case(g, 0);
        std::vector<uint8_t> out(texcreator::output_size(c.job));
        double best_us = 1e30, best_cyc = 1e30;
        for (int round = 0; round < 5; round++) {
            ULONG64 c0 = 0, c1 = 0;
            QueryThreadCycleTime(GetCurrentThread(), &c0);
            auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < reps; i++) {
                std::vector<uint8_t> p = c.param;
                texcreator::run_job(p.data(), c.job, out.data());
            }
            auto t1 = std::chrono::steady_clock::now();
            QueryThreadCycleTime(GetCurrentThread(), &c1);
            best_us = std::min(best_us, std::chrono::duration<double, std::micro>(t1 - t0).count() / reps);
            best_cyc = std::min(best_cyc, double(c1 - c0) / reps);
        }
        double samples = c.job.width * c.job.rows * (c.job.depth - 1) * 4.0;
        std::printf("native run_job %ux%ux%u rows %u: best %.1f us, %.0f thread cycles per job (%.1f cycles per noise sample)\n", c.job.width, c.job.height, c.job.depth, c.job.rows, best_us, best_cyc, best_cyc / samples);
        return 0;
    }
    if (cmd == "mem") {
        size_t ji = std::stoul(args.at(1));
        uint32_t lo = std::stoul(args.at(2), nullptr, 16), hi = std::stoul(args.at(3), nullptr, 16);
        std::vector<TraceRec> tr;
        tr.reserve(2500000);
        g_trace = &tr;
        texcreator::Job job = texcreator::parse_args(jobs.at(ji).args.data());
        run_spu(jobs[ji].param_in.data(), job);
        g_trace = nullptr;
        std::map<std::pair<uint32_t, uint32_t>, std::pair<size_t, int>> acc;
        for (auto& t : tr) {
            spudec::Insn in = spudec::decode(t.w);
            if (t.mem == ~0u || t.mem < lo || t.mem >= hi) continue;
            auto& e = acc[{t.pc, t.mem}];
            e.first++;
            e.second = in.store ? 1 : 0;
        }
        for (auto& [k, v] : acc) std::printf("%05x %s %05x x%zu\n", k.first, v.second ? "ST" : "LD", k.second, v.first);
        return 0;
    }
    if (cmd == "annot") {
        size_t ji = std::stoul(args.at(1));
        size_t keep = args.size() > 2 ? std::stoul(args[2]) : 3;
        std::vector<TraceRec> tr;
        tr.reserve(2500000);
        g_trace = &tr;
        texcreator::Job job = texcreator::parse_args(jobs.at(ji).args.data());
        run_spu(jobs[ji].param_in.data(), job);
        g_trace = nullptr;
        std::map<uint32_t, std::vector<spu::v128>> vals;
        std::map<uint32_t, size_t> count;
        for (auto& t : tr) {
            spudec::Insn in = spudec::decode(t.w);
            count[t.pc]++;
            if (in.dst < 0 && !in.store) continue;
            auto& v = vals[t.pc];
            bool seen = false;
            for (auto& x : v)
                if (std::memcmp(&x, &t.val, 16) == 0) seen = true;
            if (!seen && v.size() < keep) v.push_back(t.val);
        }
        for (auto& [pc, n] : count) {
            std::printf("%05x %zu", pc, n);
            for (auto& v : vals[pc]) std::printf(" %08x:%08x:%08x:%08x", v.u32[3], v.u32[2], v.u32[1], v.u32[0]);
            std::printf("\n");
        }
        return 0;
    }
    if (cmd == "trace") {
        size_t ji = std::stoul(args.at(1));
        std::vector<uint32_t> pcs;
        std::stringstream ss(args.at(2));
        std::string t;
        while (std::getline(ss, t, ',')) pcs.push_back(std::stoul(t, nullptr, 16));
        TraceCtx tc;
        tc.hits = &g_hits;
        if (args.size() > 3) tc.max_hits = std::stoul(args[3]);
        if (args.size() > 4) tc.lo = std::stoi(args[4]);
        if (args.size() > 5) tc.hi = std::stoi(args[5]);
        texcreator::Job job = texcreator::parse_args(jobs.at(ji).args.data());
        SpuRun r = run_spu(jobs[ji].param_in.data(), job, pcs, trace_probe, &tc);
        std::printf("insns %llu ok %d\n", (unsigned long long)r.insns, r.ok);
        return 0;
    }
    if (cmd != "all") {
        std::fprintf(stderr, "usage: tc_harness [all|dataset|synth N seed modes|noise N seed|volume N seed|coverage N seed|bench reps|annot job keep|slice job addr stoppcs|mem job lo hi|trace job pcs hits lo hi|synthdbg idx seed]\n");
        return 2;
    }
    int fails = cmd_dataset(jobs) ? 1 : 0;
    fails += cmd_noise({"noise", "5000", "7"});
    fails += cmd_synth({"synth", "50", "1", "5"});
    return fails ? 1 : 0;
}
