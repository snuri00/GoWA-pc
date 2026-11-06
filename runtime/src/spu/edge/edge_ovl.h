#pragma once

#include "spu/context.h"

#include <cstddef>

#include <cstdint>

namespace edge::ovl {

constexpr uint32_t kLo = 0x10c80;
constexpr uint32_t kHi = 0x29000;

struct Entry;

const Entry* find(const uint8_t* ls, uint32_t pc);
bool run(spu::Context& c, spu::Host& h, const Entry* e);
int index_of(const Entry* e);
const Entry* entry(size_t i);
size_t entry_count();
const char* entry_name(size_t i);

enum Counter { kRuns, kRunInsns, kDeclines, kInterpBlocks, kInterpInsns, kCounterCount };
void count(Counter k, uint64_t v);
void count_entry(int idx);
void count_decline(int native);
void capture_decline(int native, const uint8_t* ls, const spu::Context& c);
bool capture_on();
void note_interp(uint32_t pc);
bool stats_on();

}
