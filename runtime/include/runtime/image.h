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
    uint32_t image;
};

struct ExportEntry {
    const char* module;
    uint32_t nid;
    const char* name;
    uint32_t image;
    uint32_t opd;
};

struct ImageEntry {
    const char* name;
    const char* file;
    bool prx;
    uint32_t text_lo;
    uint32_t text_hi;
    uint32_t toc;
    uint32_t entry;
};

extern const ImageEntry images[];
extern const uint32_t image_count;
extern const FuncEntry functions[];
extern const uint32_t function_count;
extern const ImportEntry imports[];
extern const uint32_t import_count;
extern const ExportEntry exports[];
extern const uint32_t export_count;

}
