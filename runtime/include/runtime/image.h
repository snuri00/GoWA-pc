#pragma once

#include "ppu/context.h"

namespace image {

struct FuncEntry {
    uint32_t addr;
    ppu::Func fn;
};

struct ImportEntry {
    const char* module;
    uint32_t nid;
    const char* name;
};

extern const uint32_t text_lo;
extern const uint32_t text_hi;
extern const uint32_t toc;
extern const uint32_t entry_opd;
extern const FuncEntry functions[];
extern const uint32_t function_count;
extern const ImportEntry imports[];
extern const uint32_t import_count;

}
