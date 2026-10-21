#pragma once

#include "edge_adapters.h"
#include "edge_native.h"
#include "spu/context.h"

#include <cstdint>
#include <map>

using NativeFn = bool (*)(uint8_t* ls, spu::Context& c, edge::Outputs& outs);

std::map<uint32_t, NativeFn>& natives();

struct NativeReg {
    NativeReg(uint32_t fn, NativeFn f) { natives()[fn] = f; }
};

#include <string>
#include <vector>

using CommandFn = int (*)(const std::vector<std::string>& args);

std::map<std::string, CommandFn>& commands();

struct CommandReg {
    CommandReg(const char* name, CommandFn f) { commands()[name] = f; }
};

namespace harness {
bool edge_loaded(const uint8_t* ls);
std::vector<std::string> cap_list(const std::vector<std::string>& args, size_t from);
const std::vector<uint8_t>& elf();
}
