#pragma once

#include <atomic>
#include <cstdint>

namespace ppu {
struct Context;
}

namespace ppujobs {

bool enabled();
bool on_submit(ppu::Context& ctx);
bool on_push(ppu::Context& ctx);
bool on_kick(ppu::Context& ctx);
bool on_wait(ppu::Context& ctx);

extern std::atomic<uint32_t> g_queued;
int donor_slot();
void donate();

}
