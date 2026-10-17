#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gpu {

struct FragmentProgramKey {
    uint32_t control = 0;
    uint32_t texture_dims = 0;
    uint32_t unnormalized = 0;
    uint32_t mrt_count = 1;
};

struct FragmentProgramInfo {
    std::string glsl;
    std::vector<uint32_t> literal_slots;
    uint32_t texture_mask = 0;
    uint32_t length_slots = 0;
    uint32_t start_slot = 0;
    uint64_t hash = 0;
    bool valid = false;
};

using FpWordReader = std::function<uint32_t(uint32_t word_index)>;

FragmentProgramInfo analyse_fragment_program(const FpWordReader& read);
FragmentProgramInfo decompile_fragment_program(const FpWordReader& read, const FragmentProgramKey& key);

}
