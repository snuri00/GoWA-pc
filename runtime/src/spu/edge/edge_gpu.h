#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace edge::gpu {

enum Mode : int { kOff = 0, kDefer = 1, kVerify = 2, kPoison = 3 };

int mode();

struct Record {
    uint32_t ea = 0;
    uint32_t nv = 0;
    uint32_t off[3] = {};
    uint32_t wid[3] = {};
    uint32_t frac[3] = {};
    uint32_t m[4][4] = {};
    std::vector<uint8_t> stream;
    uint64_t stamp = 0;
};

std::shared_ptr<const Record> find(uint32_t ea);
void purge(uint64_t keep);
uint64_t publish_count();

struct Stats {
    uint64_t deferred, confirmed, published, put_skipped, materialized[8], dropped;
};
Stats stats();

extern thread_local bool t_armed;
extern const bool g_active;
bool on_put(uint8_t* ls, uint32_t lsa, uint32_t ea, uint32_t size);

void job_start();
void* state_save();
void state_restore(void* s);
bool defer_decompress(uint8_t* ls, uint32_t ctx, uint32_t verts, uint32_t nv, uint32_t desc);
void poison_table(uint8_t* ls);
bool defer_transform(uint8_t* ls, uint32_t count, uint32_t src, uint32_t dst, uint32_t matrix, uint32_t r3_out[4]);
void before_compress(uint8_t* ls, uint32_t ctx, int id);
void after_compress(uint8_t* ls, uint32_t ctx);
void touch(uint8_t* ls, uint32_t lo, uint32_t hi, int reason);
void touch_all(uint8_t* ls, int reason);

void decode_fixed(const uint8_t* stream, uint32_t v, const uint32_t off[3], const uint32_t wid[3], const uint32_t frac[3], float out[3]);
void transform_point(const float in[4], const uint32_t m[4][4], float out[4]);

}
