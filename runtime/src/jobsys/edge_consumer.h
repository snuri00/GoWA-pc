#pragma once

namespace ppu {
struct Context;
}

namespace jsedge {

bool enabled();
void init();
bool on_wait_previous(ppu::Context& ctx);

}
