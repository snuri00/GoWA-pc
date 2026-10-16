#pragma once

#include <cstdint>

namespace gpu {

bool init();
void shutdown();
void set_display_buffer(uint32_t id, uint32_t offset, uint32_t pitch, uint32_t width, uint32_t height);
void flip(uint32_t id);
bool enabled();

}
