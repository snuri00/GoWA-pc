#pragma once

#include <cstdint>
#include <string>

namespace ppu {
struct Context;
}

namespace hooks {

void init();
const std::string& name_of(uint32_t addr);
void capture(const char* reason);
void tick_patches();
bool fps_unlock_active();
double fps_unlock_target();
bool fps_unlock_timestep(ppu::Context& ctx);

}
