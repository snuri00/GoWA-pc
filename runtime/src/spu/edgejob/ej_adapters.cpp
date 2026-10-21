#include "ej_adapters.h"

#include "ej_gen.h"

namespace edgejob {

namespace {

uint32_t rbe32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

}

bool image_loaded(const uint8_t* ls) {
    for (uint32_t i = 0; i < pm::gen::ej::kHeaderWords; i++)
        if (rbe32(ls + pm::gen::ej::kImageLs + 4 * i) != pm::gen::ej::kHeader[i]) return false;
    return true;
}

bool run_hook(spu::Context& c, spu::Host& h) {
    if (!image_loaded(c.ls)) return false;
    uint64_t budget = c.executed ? c.executed : kDefaultBudget;
    return pm::gen::ej::run(c, h, budget);
}

const uint32_t* hooks(size_t& count) {
    count = pm::gen::ej::kHookCount;
    return pm::gen::ej::kHooks;
}

const uint32_t* signature(uint32_t lsa, uint32_t& words) {
    words = (pm::gen::ej::kCodeEnd - lsa) / 4;
    if (words > 16) words = 16;
    return pm::gen::ej::kCode + (lsa - pm::gen::ej::kImageLs) / 4;
}

}
