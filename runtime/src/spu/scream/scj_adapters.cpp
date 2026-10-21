#include "scj_adapters.h"

#include "scj_aux_gen.h"
#include "scj_mp3_gen.h"

namespace screamjob {

namespace {

uint32_t rbe32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

template <class NS>
struct Img {
    static bool loaded(const uint8_t* ls) {
        for (uint32_t i = 0; i < NS::kHeaderWords; i++)
            if (rbe32(ls + NS::kImageLs + 4 * i) != NS::kHeader[i]) return false;
        return true;
    }
    static bool run(spu::Context& c, spu::Host& h) {
        if (!loaded(c.ls)) return false;
        return NS::run(c, h, c.executed ? c.executed : kDefaultBudget);
    }
    static const uint32_t* hook_list(size_t& n) {
        n = NS::kHookCount;
        return NS::kHooks;
    }
    static const uint32_t* signature(uint32_t lsa, uint32_t& words) {
        words = (NS::kCodeEnd - lsa) / 4;
        if (words > 16) words = 16;
        return NS::kCode + (lsa - NS::kImageLs) / 4;
    }
};

struct Mp3 {
    static constexpr uint32_t kImageLs = pm::gen::scj_mp3::kImageLs;
    static constexpr uint32_t kHeaderWords = pm::gen::scj_mp3::kHeaderWords;
    static constexpr uint32_t kCodeEnd = pm::gen::scj_mp3::kCodeEnd;
    static constexpr const uint32_t* kHeader = pm::gen::scj_mp3::kHeader;
    static constexpr const uint32_t* kCode = pm::gen::scj_mp3::kCode;
    static constexpr const uint32_t* kHooks = pm::gen::scj_mp3::kHooks;
    static const size_t& kHookCount;
    static bool run(spu::Context& c, spu::Host& h, uint64_t b) { return pm::gen::scj_mp3::run(c, h, b); }
};

struct Aux {
    static constexpr uint32_t kImageLs = pm::gen::scj_aux::kImageLs;
    static constexpr uint32_t kHeaderWords = pm::gen::scj_aux::kHeaderWords;
    static constexpr uint32_t kCodeEnd = pm::gen::scj_aux::kCodeEnd;
    static constexpr const uint32_t* kHeader = pm::gen::scj_aux::kHeader;
    static constexpr const uint32_t* kCode = pm::gen::scj_aux::kCode;
    static constexpr const uint32_t* kHooks = pm::gen::scj_aux::kHooks;
    static const size_t& kHookCount;
    static bool run(spu::Context& c, spu::Host& h, uint64_t b) { return pm::gen::scj_aux::run(c, h, b); }
};

const size_t& Mp3::kHookCount = pm::gen::scj_mp3::kHookCount;
const size_t& Aux::kHookCount = pm::gen::scj_aux::kHookCount;

}

const tasks::Image* task_images(size_t& count) {
    static const tasks::Image kTasks[] = {
        {"ScreamJob.mp3", Img<Mp3>::hook_list, Img<Mp3>::signature, Img<Mp3>::loaded, Img<Mp3>::run, Mp3::kImageLs, Mp3::kCodeEnd},
        {"ScreamJob.aux", Img<Aux>::hook_list, Img<Aux>::signature, Img<Aux>::loaded, Img<Aux>::run, Aux::kImageLs, Aux::kCodeEnd},
    };
    count = 2;
    return kTasks;
}

const Image* images(size_t& count) {
    static const Image kImages[] = {
        {"ScreamJob.mp3", pm::gen::scj_mp3::kHooks, pm::gen::scj_mp3::kHookCount, Img<Mp3>::signature},
        {"ScreamJob.aux", pm::gen::scj_aux::kHooks, pm::gen::scj_aux::kHookCount, Img<Aux>::signature},
    };
    count = 2;
    return kImages;
}

bool run_hook(spu::Context& c, spu::Host& h) {
    uint64_t budget = c.executed ? c.executed : kDefaultBudget;
    if (Img<Mp3>::loaded(c.ls)) return pm::gen::scj_mp3::run(c, h, budget);
    if (Img<Aux>::loaded(c.ls)) return pm::gen::scj_aux::run(c, h, budget);
    return false;
}

}
