#pragma once

#include "ppu/context.h"

struct ConformanceTest {
    const char* mnemonic;
    uint32_t word;
    uint32_t base_reg;
    ppu::Func fn;
};

extern const ConformanceTest conformance_tests[];
extern const uint32_t conformance_test_count;
extern const uint32_t conformance_states;
