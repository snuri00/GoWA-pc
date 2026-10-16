#pragma once

#include <atomic>
#include <cstdint>

namespace rsv {

std::atomic<uint64_t>& version(uint32_t addr);
uint64_t snapshot(uint32_t line, uint8_t* out);
bool commit_if(uint32_t line, uint64_t rtime, const uint8_t* expected, const uint8_t* data, uint32_t offset, uint32_t size);
void store_unconditional(uint32_t line, const uint8_t* data);
void store_locked(uint32_t addr, const void* data, uint32_t size);
bool lost(uint32_t line, uint64_t rtime, const uint8_t* rdata);
void wait_change(uint32_t line, uint64_t rtime, uint32_t micros);

}
