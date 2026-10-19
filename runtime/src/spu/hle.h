#pragma once

#include "spu/context.h"

#include <cstdint>

namespace spu::hle {

using Handler = bool (*)(Context& c);

struct Function {
    uint32_t lsa;
    uint32_t sig_words;
    const uint32_t* signature;
    Handler handler;
    const char* name;
};

void register_function(const Function& f);
const Function* lookup(const uint8_t* ls, uint32_t pc);
extern uint16_t g_first[LS_SIZE / 4];
inline bool any_at(uint32_t pc) { return g_first[(pc & PC_MASK) >> 2] != 0; }
void install_builtin();

}
