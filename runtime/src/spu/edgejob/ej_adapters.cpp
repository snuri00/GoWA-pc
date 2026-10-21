#include "ej_adapters.h"

#include "ej_gen.h"

#include <cstring>

namespace edgejob {

namespace {

struct HeaderBytes {
    uint8_t b[4 * pm::gen::ej::kHeaderWords];
    HeaderBytes() {
        for (uint32_t i = 0; i < pm::gen::ej::kHeaderWords; i++)
            for (int k = 0; k < 4; k++) b[4 * i + k] = uint8_t(pm::gen::ej::kHeader[i] >> (24 - 8 * k));
    }
};

const HeaderBytes hb;

}

bool image_loaded(const uint8_t* ls) {
    return std::memcmp(ls + pm::gen::ej::kImageLs, hb.b, sizeof hb.b) == 0;
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
