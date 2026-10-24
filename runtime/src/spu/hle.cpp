#include "hle.h"

#include "runtime/log.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace spu::hle {

uint16_t g_first[LS_SIZE / 4] = {};
uint8_t g_max_words[LS_SIZE / 4] = {};

namespace {

std::vector<Function> g_functions;
std::vector<uint16_t> g_next;
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
    if (g_functions.size() >= 0xFFFE) return;
    g_functions.push_back(f);
    uint32_t slot = (f.lsa & PC_MASK) >> 2;
    g_next.push_back(g_first[slot]);
    g_first[slot] = static_cast<uint16_t>(g_functions.size());
    uint32_t words = f.sig_words > 255 ? 255 : f.sig_words;
    if (words > g_max_words[slot]) g_max_words[slot] = static_cast<uint8_t>(words);
    LOG_INFO("spuhle", "registered native SPU function %s at LS 0x%05x", f.name, f.lsa);
}

const Function* lookup_next(const uint8_t* ls, uint32_t pc, const Function* after) {
    bool past = after == nullptr;
    for (uint32_t i = g_first[(pc & PC_MASK) >> 2]; i; i = g_next[i - 1]) {
        const Function& f = g_functions[i - 1];
        if (!past) {
            past = &f == after;
            continue;
        }
        if (f.lsa != pc || pc + f.sig_words * 4 > LS_SIZE) continue;
        bool match = true;
        if (!f.signature) match = code_hash(ls + pc, f.sig_words) == f.sig_hash;
        for (uint32_t i = 0; f.signature && i < f.sig_words && match; i++) match = load_word(ls, pc + i * 4) == f.signature[i];
        if (match) return &f;
    }
    return nullptr;
}

const Function* lookup(const uint8_t* ls, uint32_t pc) { return lookup_next(ls, pc, nullptr); }

}
