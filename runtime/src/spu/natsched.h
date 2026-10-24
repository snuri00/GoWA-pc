#pragma once

#include <cstdint>
#include <memory>

namespace spu {

struct Group;
struct SpuThread;

namespace natsched {

int aux_count();
void on_group_start(const std::shared_ptr<Group>& g);
bool pm_entry_allowed(SpuThread& t);
bool aux_wait(SpuThread& t, uint32_t line, uint64_t rtime, const uint8_t* rdata, uint32_t micros);
void status(char* out, size_t size);

}

}
