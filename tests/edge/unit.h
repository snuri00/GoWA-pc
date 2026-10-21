#pragma once

#include "capture.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace edgeunit {

struct Snapshot {
    std::vector<uint8_t> ls;
    spu::Context regs{};
};

struct Hit {
    std::string cap;
    int job = 0;
    uint64_t insn = 0;
    uint64_t fn_insns = 0;
    uint32_t fn = 0;
    uint32_t ret = 0;
    uint32_t sp = 0;
    bool pure = true;
    Snapshot before;
    Snapshot after;
};

using HitFn = std::function<bool(Hit&)>;

void for_each_call(const std::vector<std::string>& caps, uint32_t fn, const HitFn& cb, const std::function<bool(const uint8_t*)>& image_ok);

struct IsoResult {
    bool ok = false;
    uint64_t insns = 0;
    std::string error;
};

IsoResult run_isolated(Snapshot& s, uint32_t ret, uint32_t sp, uint64_t max_insns = 50000000);

struct LsDiff {
    size_t bytes = 0;
    uint32_t first = 0;
    uint32_t last = 0;
};

LsDiff diff_ls(const uint8_t* a, const uint8_t* b, uint32_t sp, uint32_t stack_window = 0x10000);

}
