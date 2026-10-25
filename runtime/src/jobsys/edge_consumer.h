#pragma once

namespace ppu {
struct Context;
}

namespace jsedge {

bool enabled();
void init();
bool on_wait_previous(ppu::Context& ctx);
void set_prev_guard(bool (*g)(ppu::Context&));
bool on_set_priorities(ppu::Context& ctx);
bool spu_idle(unsigned slot);

}
