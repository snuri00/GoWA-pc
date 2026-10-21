#include "tk_adapters.h"

#include "tk_particle_gen.h"
#include "tk_t6c3e00_gen.h"
#include "tk_zlib_gen.h"

namespace tasks {

namespace {

uint32_t rbe32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

constexpr uint32_t kTailWords = 16;

template <class NS>
struct Adapter {
    static bool image_loaded(const uint8_t* ls) {
        for (uint32_t i = 0; i < NS::header_words(); i++)
            if (rbe32(ls + NS::image_ls() + 4 * i) != NS::header()[i]) return false;
        uint32_t words = (NS::code_end() - NS::image_ls()) / 4;
        for (uint32_t i = words - kTailWords; i < words; i++)
            if (rbe32(ls + NS::image_ls() + 4 * i) != NS::code()[i]) return false;
        return true;
    }
    static bool run_hook(spu::Context& c, spu::Host& h) {
        if (!image_loaded(c.ls)) return false;
        uint64_t budget = c.executed ? c.executed : kDefaultBudget;
        return NS::run(c, h, budget);
    }
    static const uint32_t* hooks(size_t& count) {
        count = NS::hook_count();
        return NS::hook_list();
    }
    static const uint32_t* signature(uint32_t lsa, uint32_t& words) {
        words = (NS::code_end() - lsa) / 4;
        if (words > 16) words = 16;
        return NS::code() + (lsa - NS::image_ls()) / 4;
    }
};

#define TASK_NS(TAG, N)                                                                   \
    struct TAG {                                                                          \
        static uint32_t image_ls() { return pm::gen::N::kImageLs; }                       \
        static uint32_t header_words() { return pm::gen::N::kHeaderWords; }               \
        static uint32_t code_end() { return pm::gen::N::kCodeEnd; }                       \
        static const uint32_t* header() { return pm::gen::N::kHeader; }                   \
        static const uint32_t* code() { return pm::gen::N::kCode; }                       \
        static const uint32_t* hook_list() { return pm::gen::N::kHooks; }                 \
        static size_t hook_count() { return pm::gen::N::kHookCount; }                     \
        static bool run(spu::Context& c, spu::Host& h, uint64_t b) { return pm::gen::N::run(c, h, b); } \
    };

TASK_NS(Zlib, tk_zlib)
TASK_NS(Particle, tk_particle)
TASK_NS(T6c3e00, tk_t6c3e00)

#undef TASK_NS

const Image kImages[] = {
    {"Task.zlib", Adapter<Zlib>::hooks, Adapter<Zlib>::signature, Adapter<Zlib>::image_loaded, Adapter<Zlib>::run_hook, pm::gen::tk_zlib::kImageLs, pm::gen::tk_zlib::kCodeEnd},
    {"Task.particle", Adapter<Particle>::hooks, Adapter<Particle>::signature, Adapter<Particle>::image_loaded, Adapter<Particle>::run_hook, pm::gen::tk_particle::kImageLs, pm::gen::tk_particle::kCodeEnd},
    {"Task.6c3e00", Adapter<T6c3e00>::hooks, Adapter<T6c3e00>::signature, Adapter<T6c3e00>::image_loaded, Adapter<T6c3e00>::run_hook, pm::gen::tk_t6c3e00::kImageLs, pm::gen::tk_t6c3e00::kCodeEnd},
};

}

const Image* images(size_t& count) {
    count = sizeof(kImages) / sizeof(kImages[0]);
    return kImages;
}

}
