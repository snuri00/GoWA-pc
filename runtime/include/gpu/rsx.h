#pragma once

#include <cstdint>

namespace gpu::rsx {

void method(uint32_t reg, uint32_t arg);
uint32_t method_run(uint32_t reg, const uint32_t* args, uint32_t n);
bool present(uint32_t buffer_id);
uint32_t reg(uint32_t method);
void frame_complete(bool all);
uint32_t needs_full_wait();
uint32_t zpass_value(uint32_t ea);
void sync_decodes();

}
