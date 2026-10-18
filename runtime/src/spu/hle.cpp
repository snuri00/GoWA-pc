#include "hle.h"

#include "runtime/log.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace spu::hle {

namespace {

std::array<uint8_t, LS_SIZE / 4> g_flags{};
std::vector<Function> g_functions;
std::mutex g_lock;

uint32_t load_word(const uint8_t* ls, uint32_t a) {
    uint32_t w;
    std::memcpy(&w, ls + a, 4);
    return __builtin_bswap32(w);
}

}

void register_function(const Function& f) {
    if (const char* e = std::getenv("GOWA_SPU_HLE"); e && e[0] == '0') return;
    std::lock_guard lk(g_lock);
    g_functions.push_back(f);
    g_flags[(f.lsa & PC_MASK) >> 2] = 1;
    LOG_INFO("spuhle", "registered native SPU function %s at LS 0x%05x", f.name, f.lsa);
}

bool any_at(uint32_t pc) { return g_flags[(pc & PC_MASK) >> 2] != 0; }

const Function* lookup(const uint8_t* ls, uint32_t pc) {
    for (const Function& f : g_functions) {
        if (f.lsa != pc || pc + f.sig_words * 4 > LS_SIZE) continue;
        bool match = true;
        for (uint32_t i = 0; i < f.sig_words && match; i++) match = load_word(ls, pc + i * 4) == f.signature[i];
        if (match) return &f;
    }
    return nullptr;
}

}
