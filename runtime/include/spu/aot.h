#pragma once

#include "spu/context.h"

#include <cstddef>
#include <cstdint>

namespace spu {

using BlockFn = bool (*)(Context&, Host&);

struct CompiledBlock {
    uint32_t lsa;
    uint32_t words;
    const uint32_t* code;
    BlockFn fn;
    bool pure;
};

const CompiledBlock* compiled_blocks(size_t& count);

namespace aot {

constexpr uint32_t MAX_BLOCK_WORDS = 256;

bool active();
bool recording();
bool is_terminator(uint32_t word);
const CompiledBlock* find(const uint8_t* ls, uint32_t pc);
void record(const uint8_t* ls, uint32_t pc);
void flush();

}

}
