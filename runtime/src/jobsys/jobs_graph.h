#pragma once

#include <cstdint>

namespace ppu {
struct Context;
}

namespace wj {

void install(void (*hook)(uint32_t addr, bool (*guard)(ppu::Context&)));

}
