#pragma once

#include <cstdint>

namespace gpu::rsx {

void method(uint32_t reg, uint32_t arg);
bool present(uint32_t buffer_id);
uint32_t reg(uint32_t method);
void frame_complete();

}
