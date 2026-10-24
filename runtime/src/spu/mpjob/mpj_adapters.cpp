#include "mpj_adapters.h"

#include "gvj_gen.h"
#include "mpj_gen.h"
#include "pbj_gen.h"
#include "spj_gen.h"

namespace mpjob {

namespace {

uint32_t rbe32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

struct Info {
    const char* name;
    uint32_t image_ls, header_words, code_end;
    const uint32_t* header;
    const uint32_t* code;
    const uint32_t* hooks;
    const size_t* hook_count;
    bool (*run)(spu::Context&, spu::Host&, uint64_t);
};

#define MPJ_INFO(label, ns) {label, ns::kImageLs, ns::kHeaderWords, ns::kCodeEnd, ns::kHeader, ns::kCode, ns::kHooks, &ns::kHookCount, ns::run}

const Info kInfo[kJobCount] = {
    MPJ_INFO("MeshPrepJob", pm::gen::mpj),
    MPJ_INFO("PrimBuilderJob", pm::gen::pbj),
    MPJ_INFO("GroupVisJob", pm::gen::gvj),
    MPJ_INFO("SortPrimJob", pm::gen::spj),
};

}

const char* name(int job) { return kInfo[job].name; }

bool image_loaded(int job, const uint8_t* ls) {
    const Info& f = kInfo[job];
    for (uint32_t i = 0; i < f.header_words; i++)
        if (rbe32(ls + f.image_ls + 4 * i) != f.header[i]) return false;
    return true;
}

bool run(int job, spu::Context& c, spu::Host& h) {
    if (!image_loaded(job, c.ls)) return false;
    uint64_t budget = c.executed ? c.executed : kDefaultBudget;
    return kInfo[job].run(c, h, budget);
}

const uint32_t* hooks(int job, size_t& count) {
    count = *kInfo[job].hook_count;
    return kInfo[job].hooks;
}

const uint32_t* signature(int job, uint32_t lsa, uint32_t& words) {
    const Info& f = kInfo[job];
    words = (f.code_end - lsa) / 4;
    if (words > 16) words = 16;
    return f.code + (lsa - f.image_ls) / 4;
}

}
