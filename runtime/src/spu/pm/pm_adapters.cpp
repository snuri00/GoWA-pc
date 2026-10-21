#include "pm_adapters.h"

#include "pm_gen.h"

#include <cstring>

namespace pm {

namespace {

uint32_t rbe32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

uint32_t code_range_end(uint32_t lsa) { return lsa < 0xaf0 ? 0xaf0 : gen::kCodeEnd; }

}

bool image_loaded(const uint8_t* ls) {
    for (uint32_t i = 0; i < gen::kHeaderWords; i++)
        if (rbe32(ls + gen::kImageLs + 4 * i) != gen::kHeader[i]) return false;
    return true;
}

bool run_hook(spu::Context& c, spu::Host& h) {
    if (!image_loaded(c.ls)) return false;
    uint64_t budget = c.executed ? c.executed : kDefaultBudget;
    return gen::run(c, h, budget);
}

const uint32_t* hooks(size_t& count) {
    count = gen::kHookCount;
    return gen::kHooks;
}

const uint32_t* signature(uint32_t lsa, uint32_t& words) {
    uint32_t end = code_range_end(lsa);
    words = (end - lsa) / 4;
    if (words > 16) words = 16;
    return gen::kCode + (lsa - gen::kImageLs) / 4;
}

}
