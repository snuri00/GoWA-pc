#pragma once

namespace spu {
struct Context;
}

namespace wj {

void install_inplace();
bool selected(int job);
extern void (*g_prep_diff)(int job, spu::Context& c);

}
