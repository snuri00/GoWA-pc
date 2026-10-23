#include "edge_gpu.h"

#include "spu_float.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <map>

namespace edge::gpu {

namespace {

constexpr uint32_t kLs = 0x40000;
constexpr uint32_t kMask = kLs - 1;

int env_mode() {
    const char* e = std::getenv("GOWA_EDGE_GPU");
    return e ? std::atoi(e) : 0;
}

const int g_mode = env_mode();

std::atomic<uint64_t> s_deferred{0}, s_confirmed{0}, s_published{0}, s_put_skipped{0}, s_dropped{0};
std::atomic<uint64_t> s_mat[8]{};

std::mutex g_m;
std::map<uint32_t, std::shared_ptr<const Record>> g_records;
std::atomic<uint32_t> g_lo{0xFFFFFFFFu}, g_hi{0};

void erase_range_locked(uint32_t ea, uint32_t size) {
    auto it = g_records.lower_bound(ea >= 0x10000 ? ea - 0x10000 : 0);
    while (it != g_records.end() && it->first < ea + size) {
        if (it->first + 12 * it->second->nv > ea) it = g_records.erase(it);
        else ++it;
    }
}

void invalidate(uint32_t ea, uint32_t size) {
    if (ea >= g_hi.load(std::memory_order_relaxed) || ea + size <= g_lo.load(std::memory_order_relaxed)) return;
    std::lock_guard lk(g_m);
    erase_range_locked(ea, size);
}
std::atomic<uint64_t> g_stamp{0};

enum State { kNone, kDecoded, kTransformed, kAwaitPut };

struct Pending {
    State st = kNone;
    uint32_t ctx = 0, table = 0, nv = 0, n2 = 0, count = 0;
    uint32_t out_lsa = 0, out_bytes = 0;
    std::shared_ptr<Record> rec;
};

thread_local Pending t_p;

uint32_t rd32(const uint8_t* ls, uint32_t a) { return spuf::ld32(ls + (a & kMask)); }
void wr32(uint8_t* ls, uint32_t a, uint32_t v) { spuf::st32(ls + (a & kMask), v); }

uint64_t window64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

float fixed_to_float(uint32_t field, uint32_t offset, uint32_t frac) {
    uint32_t sum = field + offset;
    int32_t ip = frac >= 32 ? (int32_t(sum) >> 31) : (int32_t(sum) >> frac);
    uint32_t sh = (32 - frac) & 63;
    uint32_t fp = sh >= 32 ? 0 : (sum << sh);
    float a = spuf::fl(spuf::csflt(ip, 155));
    float b = spuf::fl(spuf::cuflt(fp, 123));
    return spuf::fa(a, b);
}

bool big(uint32_t w) { return (w & 0x7FFFFFFF) > 0x5D7FFFFF; }

void reset() {
    t_p = Pending{};
    t_armed = false;
}

void decode_table(uint8_t* ls, const Pending& p) {
    const Record& r = *p.rec;
    for (uint32_t v = 0; v < p.n2; v++) {
        float o[3];
        decode_fixed(r.stream.data(), v, r.off, r.wid, r.frac, o);
        uint32_t a = (p.table + 16 * v) & kMask;
        for (int c = 0; c < 3; c++) spuf::stf(ls + a + 4 * c, o[c]);
        spuf::stf(ls + a + 12, 1.0f);
    }
}

void transform_table(uint8_t* ls, const Pending& p) {
    uint32_t groups = (p.count + 7) >> 3;
    for (uint32_t v = 0; v < groups * 8; v++) {
        uint32_t a = (p.table + 16 * v) & kMask;
        float in[4], out[4];
        for (int c = 0; c < 4; c++) in[c] = spuf::ldf(ls + ((a + 4 * c) & kMask));
        transform_point(in, p.rec->m, out);
        for (int c = 0; c < 4; c++) spuf::stf(ls + ((a + 4 * c) & kMask), out[c]);
    }
}

void write_records(uint8_t* ls, const Pending& p) {
    for (uint32_t v = 0; v < p.nv; v++) {
        uint32_t a = (p.table + 16 * v) & kMask;
        for (int c = 0; c < 3; c++) std::memcpy(ls + ((p.out_lsa + 12 * v + 4 * c) & kMask), ls + a + 4 * c, 4);
    }
}

void materialize(uint8_t* ls, int reason) {
    Pending& p = t_p;
    if (p.st == kNone) return;
    s_mat[reason & 7].fetch_add(1, std::memory_order_relaxed);
    if (g_mode != kVerify) {
        decode_table(ls, p);
        if (p.st >= kTransformed) transform_table(ls, p);
        if (p.st == kAwaitPut) write_records(ls, p);
    }
    reset();
}

}

thread_local bool t_armed = false;
const bool g_active = g_mode != kOff;

void* state_save() { return new Pending(t_p); }

void state_restore(void* s) {
    Pending* p = static_cast<Pending*>(s);
    t_p = *p;
    t_armed = t_p.st == kAwaitPut;
    delete p;
}

int mode() { return g_mode; }

void decode_fixed(const uint8_t* stream, uint32_t v, const uint32_t off[3], const uint32_t wid[3], const uint32_t frac[3], float out[3]) {
    uint64_t w = window64(stream + 12 * v);
    uint32_t pos = 0;
    for (int c = 0; c < 3; c++) {
        uint32_t width = wid[c] & 63;
        uint32_t field = width == 0 ? 0 : uint32_t((w << pos) >> (64 - width));
        pos += width;
        out[c] = fixed_to_float(field, off[c], frac[c]);
    }
}

void transform_point(const float in[4], const uint32_t m[4][4], float out[4]) {
    spuf::CsrGuard g;
    for (int i = 0; i < 4; i++) {
        float t = spuf::fm(in[i], spuf::fl(m[i][i]));
        t = spuf::fma(in[(i + 1) & 3], spuf::fl(m[i][(i + 1) & 3]), t);
        t = spuf::fma(in[(i + 2) & 3], spuf::fl(m[i][(i + 2) & 3]), t);
        out[i] = spuf::fma(in[(i + 3) & 3], spuf::fl(m[i][(i + 3) & 3]), t);
    }
}

std::shared_ptr<const Record> find(uint32_t ea) {
    std::lock_guard lk(g_m);
    auto it = g_records.find(ea);
    return it == g_records.end() ? nullptr : it->second;
}

void purge(uint64_t keep) {
    uint64_t now = g_stamp.load(std::memory_order_relaxed);
    if (now < keep) return;
    std::lock_guard lk(g_m);
    for (auto it = g_records.begin(); it != g_records.end();) {
        if (it->second->stamp + keep < now) it = g_records.erase(it);
        else ++it;
    }
}

uint64_t publish_count() { return g_stamp.load(std::memory_order_relaxed); }

Stats stats() {
    Stats s{s_deferred.load(), s_confirmed.load(), s_published.load(), s_put_skipped.load(), {}, s_dropped.load()};
    for (int i = 0; i < 8; i++) s.materialized[i] = s_mat[i].load();
    return s;
}

void job_start() {
    if (t_p.st != kNone) s_dropped.fetch_add(1, std::memory_order_relaxed);
    reset();
}

bool defer_decompress(uint8_t* ls, uint32_t ctx, uint32_t verts, uint32_t nv, uint32_t desc) {
    if (g_mode == kOff) return false;
    reset();
    if (!nv || nv > 0x2000) return false;
    uint32_t cfg0 = ls[ctx], skin = ls[ctx + 6] & 15;
    if ((cfg0 & 0x10) || skin) return false;
    uint32_t n2 = (nv + 1) & ~1u;
    uint32_t bytes = 12 * n2 + 8;
    if (verts + bytes > kLs) return false;
    uint8_t* flags = ls + ctx + 352;
    uint32_t k = 16;
    for (uint32_t i = 0; i < 16; i++)
        if (flags[i] == 0) {
            k = i;
            break;
        }
    if (k == 16) return false;
    uint32_t table = rd32(ls, ctx + 16 + 4 * k);
    if ((table & 15) || table + 16 * n2 > kLs) return false;
    auto rec = std::make_shared<Record>();
    rec->nv = nv;
    for (int c = 0; c < 3; c++) {
        rec->off[c] = rd32(ls, desc + 4 * c);
        rec->frac[c] = ls[(desc + 0x11 + 2 * c) & kMask];
        rec->wid[c] = ls[(desc + 0x10 + 2 * c) & kMask] + rec->frac[c];
    }
    if (rec->wid[0] > 32 || rec->wid[1] > 32 || rec->wid[2] > 32 || rec->wid[0] + rec->wid[1] + rec->wid[2] > 64) return false;
    rec->stream.assign(ls + verts, ls + verts + bytes);
    Pending& p = t_p;
    p.rec = std::move(rec);
    p.ctx = ctx;
    p.table = table;
    p.nv = nv;
    p.n2 = n2;
    p.st = kDecoded;
    s_deferred.fetch_add(1, std::memory_order_relaxed);
    if (g_mode != kDefer) return false;
    flags[k] = 1;
    wr32(ls, ctx + 92, table);
    return true;
}

void poison_table(uint8_t* ls) {
    Pending& p = t_p;
    if (g_mode != kPoison || p.st != kDecoded) return;
    for (uint32_t v = 0; v < 4 * p.n2; v++) wr32(ls, p.table + 4 * v, 0x7FC0DEADu);
}

bool defer_transform(uint8_t* ls, uint32_t count, uint32_t src, uint32_t dst, uint32_t matrix, uint32_t r3_out[4]) {
    Pending& p = t_p;
    if (p.st == kNone) return false;
    uint32_t lo = p.table, hi = p.table + 16 * p.n2;
    bool hits = (src < hi && src + 16 * ((count + 7) & ~7u) > lo) || (dst < hi && dst + 16 * ((count + 7) & ~7u) > lo);
    if (!hits) return false;
    bool ok = p.st == kDecoded && src == p.table && dst == p.table && ((count + 7) & ~7u) >= p.n2 && count <= p.n2 + 8;
    uint32_t m[4][4];
    for (int r = 0; r < 4 && ok; r++)
        for (int c = 0; c < 4; c++) {
            m[r][c] = rd32(ls, matrix + 16 * r + 4 * c);
            if (big(m[r][c])) ok = false;
        }
    if (!ok) {
        materialize(ls, 1);
        return false;
    }
    std::memcpy(p.rec->m, m, sizeof m);
    p.count = count;
    p.st = kTransformed;
    if (g_mode == kVerify) return false;
    if (g_mode == kPoison)
        for (uint32_t v = 0; v < 4 * p.n2; v++) wr32(ls, p.table + 4 * v, 0x7FC0DEADu);
    uint32_t groups = (count + 7) >> 3;
    uint32_t w[4];
    for (int i = 0; i < 4; i++) w[i] = rd32(ls, src + 128 * groups + 112 + 4 * i);
    for (int i = 0; i < 4; i++) r3_out[i] = w[(i + 1) & 3];
    return true;
}

void before_compress(uint8_t* ls, uint32_t ctx, int id) {
    Pending& p = t_p;
    if (p.st == kNone) return;
    if (p.st != kTransformed || id != 0 || ctx != p.ctx || g_mode == kPoison) materialize(ls, 2);
}

void after_compress(uint8_t* ls, uint32_t ctx) {
    Pending& p = t_p;
    if (p.st != kTransformed) return;
    p.out_lsa = rd32(ls, ctx + 84);
    p.out_bytes = 12 * p.nv;
    p.st = kAwaitPut;
    t_armed = true;
    s_confirmed.fetch_add(1, std::memory_order_relaxed);
}

void touch(uint8_t* ls, uint32_t lo, uint32_t hi, int reason) {
    Pending& p = t_p;
    if (p.st == kNone) return;
    if (lo < p.table + 16 * p.n2 && hi > p.table) materialize(ls, reason);
}

void touch_all(uint8_t* ls, int reason) {
    if (t_p.st != kNone) materialize(ls, reason);
}

bool on_put(uint8_t* ls, uint32_t lsa, uint32_t ea, uint32_t size) {
    Pending& p = t_p;
    if (p.st != kAwaitPut) {
        invalidate(ea, size);
        return false;
    }
    uint32_t lo = p.out_lsa, hi = p.out_lsa + p.out_bytes;
    if (!(lsa < hi && lsa + size > lo)) {
        invalidate(ea, size);
        return false;
    }
    if (lsa != lo || size < p.out_bytes || size > ((p.out_bytes + 15) & ~15u) + 16) {
        materialize(ls, 3);
        invalidate(ea, size);
        return false;
    }
    p.rec->ea = ea;
    p.rec->stamp = g_stamp.fetch_add(1, std::memory_order_relaxed) + 1;
    {
        std::lock_guard lk(g_m);
        erase_range_locked(ea, size);
        if (ea < g_lo.load(std::memory_order_relaxed)) g_lo.store(ea, std::memory_order_relaxed);
        if (ea + size > g_hi.load(std::memory_order_relaxed)) g_hi.store(ea + size, std::memory_order_relaxed);
        g_records[ea] = std::move(p.rec);
    }
    s_published.fetch_add(1, std::memory_order_relaxed);
    bool skip = g_mode == kDefer;
    if (skip) s_put_skipped.fetch_add(1, std::memory_order_relaxed);
    reset();
    return skip;
}

}
