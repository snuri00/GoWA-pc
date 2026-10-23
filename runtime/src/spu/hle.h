#pragma once

#include "spu/context.h"

#include <cstdint>

namespace spu::hle {

using Handler = bool (*)(Context& c);
using HostHandler = bool (*)(Context& c, Host& h);

struct Function {
    uint32_t lsa;
    uint32_t sig_words;
    const uint32_t* signature;
    Handler handler;
    const char* name;
    HostHandler host_handler = nullptr;
    uint64_t sig_hash = 0;
};

inline uint64_t code_hash(const uint8_t* p, uint32_t words) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < words * 4; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

void register_function(const Function& f);
const Function* lookup(const uint8_t* ls, uint32_t pc);
extern uint16_t g_first[LS_SIZE / 4];
extern uint8_t g_max_words[LS_SIZE / 4];
inline uint32_t max_sig_words(uint32_t pc) { return g_max_words[(pc & PC_MASK) >> 2]; }
inline bool any_at(uint32_t pc) { return g_first[(pc & PC_MASK) >> 2] != 0; }
void install_builtin();

}
