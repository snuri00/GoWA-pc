#pragma once

#include "spu/context.h"

#include <cstddef>
#include <cstdint>

namespace mpjob {

constexpr uint64_t kDefaultBudget = 1ull << 16;

enum Job { kMeshPrep, kPrimBuilder, kGroupVis, kSortPrim, kJobCount };

const char* name(int job);
bool image_loaded(int job, const uint8_t* ls);
const uint32_t* hooks(int job, size_t& count);
const uint32_t* signature(int job, uint32_t lsa, uint32_t& words);
bool run(int job, spu::Context& c, spu::Host& h);

template <int J>
bool run_hook(spu::Context& c, spu::Host& h) {
    return run(J, c, h);
}

template <int J>
bool loaded(const uint8_t* ls) {
    return image_loaded(J, ls);
}

template <int J>
const uint32_t* hooks_of(size_t& count) {
    return hooks(J, count);
}

template <int J>
const uint32_t* signature_of(uint32_t lsa, uint32_t& words) {
    return signature(J, lsa, words);
}

}
