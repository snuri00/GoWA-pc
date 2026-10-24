#include "hle.h"
#include "texcreator_native.h"
#include "edge/edge_adapters.h"
#include "edge/edge_ovl.h"
#include "renjob/renjob_adapters.h"
#include "scream/scream_adapters.h"
#include "scream/scj_adapters.h"
#include "cloth/cloth_adapters.h"
#include "shadows/shadows_adapters.h"
#include "meshprep/meshprep_adapters.h"
#include "pm/pm_adapters.h"
#include "edgejob/ej_adapters.h"
#include "meshhier/mh_adapters.h"
#include "anim/an_adapters.h"
#include "tasks/tk_adapters.h"
#include "mpjob/mpj_adapters.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include <mutex>

namespace spu::hle {

namespace {

constexpr uint64_t kTexCreatorComputeHash = 0xe854072b0154689cull;

bool texcreator_compute(Context& c) {
    uint32_t param = pref(c.gpr[3]) & LS_MASK;
    uint32_t out = pref(c.gpr[4]) & LS_MASK;
    texcreator::Job job;
    job.width = pref(c.gpr[5]);
    job.height = pref(c.gpr[6]);
    job.depth = pref(c.gpr[7]);
    job.rows = pref(c.gpr[8]);
    job.row_start = pref(c.gpr[9]);
    if (!job.width || !job.depth || !job.rows || job.width > 1024 || job.height > 1024 || job.depth > 64 || job.rows > 64) return false;
    if (job.row_start + job.rows > job.height) return false;
    if (param + texcreator::kParamSize > LS_SIZE || uint64_t(out) + texcreator::output_size(job) > LS_SIZE) return false;
    texcreator::run_job(c.ls + param, job, c.ls + out);
    return true;
}

template <size_t I>
bool edge_handler(Context& c) {
    if (!edge::image_loaded(c.ls)) return false;
    size_t n = 0;
    const edge::Adapter& a = edge::adapters(n)[I];
    edge::Outputs outs;
    if (a.handler(c.ls, c, outs)) return true;
    edge::ovl::count_decline(int(I));
    return false;
}

using EdgeHandler = bool (*)(Context&);

constexpr EdgeHandler kEdgeHandlers[] = {edge_handler<0>, edge_handler<1>, edge_handler<2>, edge_handler<3>, edge_handler<4>, edge_handler<5>, edge_handler<6>, edge_handler<7>, edge_handler<8>, edge_handler<9>};

bool skipped(const char* var, uint32_t lsa) {
    const char* e = std::getenv(var);
    if (!e) return false;
    std::string list = e;
    size_t p = 0;
    while (p < list.size()) {
        size_t q = list.find(',', p);
        if (q == std::string::npos) q = list.size();
        std::string tok = list.substr(p, q - p);
        if (!tok.empty() && std::strtoul(tok.c_str(), nullptr, 16) == lsa) return true;
        p = q + 1;
    }
    return false;
}

bool is_edge_native(uint32_t lsa) {
    size_t n = 0;
    const edge::Adapter* a = edge::adapters(n);
    for (size_t i = 0; i < n; i++)
        if (a[i].lsa == lsa) return true;
    return false;
}

bool edgejob_on() {
    const char* e = std::getenv("GOWA_EDGEJOB_HLE");
    return !(e && e[0] == '0');
}

void install_edge() {
    if (edgejob_on()) {
        size_t hn = 0;
        const uint32_t* h = edgejob::hooks(hn);
        for (size_t i = 0; i < hn; i++) {
            if (!is_edge_native(h[i]) || skipped("GOWA_EDGEJOB_HLE_SKIP", h[i])) continue;
            uint32_t words = 0;
            const uint32_t* sig = edgejob::signature(h[i], words);
            register_function({h[i], words, sig, nullptr, "EdgeJob.fallback", edgejob::run_hook});
        }
    }
    if (const char* e = std::getenv("GOWA_EDGE_HLE"); e && e[0] == '0') return;
    size_t n = 0;
    const edge::Adapter* a = edge::adapters(n);
    for (size_t i = 0; i < n && i < sizeof(kEdgeHandlers) / sizeof(kEdgeHandlers[0]); i++) {
        if (skipped("GOWA_EDGE_HLE_SKIP", a[i].lsa)) continue;
        register_function({a[i].lsa, a[i].sig_words, nullptr, kEdgeHandlers[i], a[i].name, nullptr, a[i].sig_hash});
    }
}

template <size_t I>
bool renjob_handler(Context& c) {
    if (!renjob::image_loaded(c.ls)) return false;
    size_t n = 0;
    return renjob::adapters(n)[I].handler(c.ls, c);
}

using RenjobHandler = bool (*)(Context&);

template <size_t... I>
constexpr auto make_renjob_handlers(std::index_sequence<I...>) {
    return std::array<RenjobHandler, sizeof...(I)>{renjob_handler<I>...};
}

constexpr auto kRenjobHandlers = make_renjob_handlers(std::make_index_sequence<160>{});

void install_renjob() {
    if (const char* e = std::getenv("GOWA_RENJOB_HLE"); e && e[0] == '0') return;
    size_t n = 0;
    const renjob::Adapter* a = renjob::adapters(n);
    for (size_t i = 0; i < n && i < kRenjobHandlers.size(); i++) {
        if (skipped("GOWA_RENJOB_HLE_SKIP", a[i].lsa)) continue;
        register_function({a[i].lsa, a[i].sig_words, a[i].signature, kRenjobHandlers[i], a[i].name});
    }
}

template <size_t I>
bool cloth_handler(Context& c) {
    if (!cloth::image_loaded(c.ls)) return false;
    size_t n = 0;
    return cloth::adapters(n)[I].handler(c.ls, c);
}

template <size_t I>
bool shadows_handler(Context& c) {
    if (!shadows::image_loaded(c.ls)) return false;
    size_t n = 0;
    return shadows::adapters(n)[I].handler(c.ls, c);
}

template <size_t... I>
constexpr auto make_cloth_handlers(std::index_sequence<I...>) {
    return std::array<Handler, sizeof...(I)>{cloth_handler<I>...};
}

template <size_t... I>
constexpr auto make_shadows_handlers(std::index_sequence<I...>) {
    return std::array<Handler, sizeof...(I)>{shadows_handler<I>...};
}

constexpr auto kClothHandlers = make_cloth_handlers(std::make_index_sequence<64>{});
constexpr auto kShadowsHandlers = make_shadows_handlers(std::make_index_sequence<64>{});

void install_cloth_shadows() {
    if (const char* e = std::getenv("GOWA_CLOTH_HLE"); !e || e[0] != '0') {
        size_t n = 0;
        const cloth::Adapter* a = cloth::adapters(n);
        for (size_t i = 0; i < n && i < kClothHandlers.size(); i++) {
            if (skipped("GOWA_CLOTH_HLE_SKIP", a[i].lsa)) continue;
            register_function({a[i].lsa, a[i].sig_words, a[i].signature, kClothHandlers[i], a[i].name});
        }
    }
    if (const char* e = std::getenv("GOWA_SHADOWS_HLE"); !e || e[0] != '0') {
        size_t n = 0;
        const shadows::Adapter* a = shadows::adapters(n);
        for (size_t i = 0; i < n && i < kShadowsHandlers.size(); i++) {
            if (skipped("GOWA_SHADOWS_HLE_SKIP", a[i].lsa)) continue;
            register_function({a[i].lsa, a[i].sig_words, a[i].signature, kShadowsHandlers[i], a[i].name});
        }
    }
}

template <size_t I>
bool scream_handler(Context& c) {
    size_t n = 0;
    return scream::adapters(n)[I].handler(c.ls, c);
}

template <size_t... I>
constexpr auto make_scream_handlers(std::index_sequence<I...>) {
    return std::array<Handler, sizeof...(I)>{scream_handler<I>...};
}

constexpr auto kScreamHandlers = make_scream_handlers(std::make_index_sequence<128>{});

void install_scream() {
    const char* e = std::getenv("GOWA_SCREAM_HLE");
    if (e && e[0] == '0') return;
    size_t n = 0;
    const scream::Adapter* a = scream::adapters(n);
    for (size_t i = 0; i < n && i < kScreamHandlers.size(); i++) {
        if (skipped("GOWA_SCREAM_HLE_SKIP", a[i].lsa)) continue;
        register_function({a[i].lsa, a[i].sig_words, a[i].signature, kScreamHandlers[i], a[i].name});
    }
}

template <size_t I>
bool meshprep_handler(Context& c) {
    size_t n = 0;
    return meshprep::adapters(n)[I].handler(c.ls, c);
}

template <size_t... I>
constexpr auto make_meshprep_handlers(std::index_sequence<I...>) {
    return std::array<Handler, sizeof...(I)>{meshprep_handler<I>...};
}

constexpr auto kMeshprepHandlers = make_meshprep_handlers(std::make_index_sequence<160>{});

void install_meshprep() {
    const char* e = std::getenv("GOWA_MESHPREP_HLE");
    if (e && e[0] == '0') return;
    size_t n = 0;
    const meshprep::Adapter* a = meshprep::adapters(n);
    for (size_t i = 0; i < n && i < kMeshprepHandlers.size(); i++) {
        if (skipped("GOWA_MESHPREP_HLE_SKIP", a[i].lsa)) continue;
        register_function({a[i].lsa, a[i].sig_words, a[i].signature, kMeshprepHandlers[i], a[i].name});
    }
}

void install_pm() {
    const char* e = std::getenv("GOWA_PM_HLE");
    if (e && e[0] == '0') return;
    size_t n = 0;
    const uint32_t* h = pm::hooks(n);
    for (size_t i = 0; i < n; i++) {
        if (skipped("GOWA_PM_HLE_SKIP", h[i])) continue;
        uint32_t words = 0;
        const uint32_t* sig = pm::signature(h[i], words);
        register_function({h[i], words, sig, nullptr, "PM", pm::run_hook});
    }
}

void install_edgejob() {
    if (!edgejob_on()) return;
    size_t n = 0;
    const uint32_t* h = edgejob::hooks(n);
    for (size_t i = 0; i < n; i++) {
        if (is_edge_native(h[i]) || skipped("GOWA_EDGEJOB_HLE_SKIP", h[i])) continue;
        uint32_t words = 0;
        const uint32_t* sig = edgejob::signature(h[i], words);
        register_function({h[i], words, sig, nullptr, "EdgeJob", edgejob::run_hook});
    }
}

void install_tasks() {
    const char* e = std::getenv("GOWA_TASK_HLE");
    if (e && e[0] == '0') return;
    size_t m = 0;
    const tasks::Image* img = tasks::images(m);
    for (size_t k = 0; k < m; k++) {
        size_t n = 0;
        const uint32_t* h = img[k].hooks(n);
        for (size_t i = 0; i < n; i++) {
            if (skipped("GOWA_TASK_HLE_SKIP", h[i])) continue;
            uint32_t words = 0;
            const uint32_t* sig = img[k].signature(h[i], words);
            register_function({h[i], words, sig, nullptr, img[k].name, img[k].run_hook});
        }
    }
}

void install_screamjob() {
    const char* e = std::getenv("GOWA_SCREAMJOB_HLE");
    if (e && e[0] == '0') return;
    size_t m = 0;
    const screamjob::Image* img = screamjob::images(m);
    for (size_t k = 0; k < m; k++) {
        for (size_t i = 0; i < img[k].hook_count; i++) {
            uint32_t lsa = img[k].hooks[i];
            if (skipped("GOWA_SCREAMJOB_HLE_SKIP", lsa)) continue;
            uint32_t words = 0;
            const uint32_t* sig = img[k].signature(lsa, words);
            register_function({lsa, words, sig, nullptr, img[k].name, screamjob::run_hook});
        }
    }
}

template <class F, class S, class R>
void install_wws_job(const char* env, const char* skip, const char* name, F hooks, S signature, R run) {
    const char* e = std::getenv(env);
    if (e && e[0] == '0') return;
    size_t n = 0;
    const uint32_t* h = hooks(n);
    for (size_t i = 0; i < n; i++) {
        if (skipped(skip, h[i])) continue;
        uint32_t words = 0;
        const uint32_t* sig = signature(h[i], words);
        register_function({h[i], words, sig, nullptr, name, run});
    }
}

template <int J>
void install_mpjob_one(const char* sel, const char* key) {
    std::string s = sel;
    if (s != "1" && s != "all" && s.find(key) == std::string::npos) return;
    size_t n = 0;
    const uint32_t* h = mpjob::hooks_of<J>(n);
    for (size_t i = 0; i < n; i++) {
        if (skipped("GOWA_MPJOB_HLE_SKIP", h[i])) continue;
        uint32_t words = 0;
        const uint32_t* sig = mpjob::signature_of<J>(h[i], words);
        register_function({h[i], words, sig, nullptr, mpjob::name(J), mpjob::run_hook<J>});
    }
}

void install_mpjob() {
    const char* e = std::getenv("GOWA_MPJOB_HLE");
    if (!e) e = "1";
    if (e[0] == '0') return;
    install_mpjob_one<mpjob::kMeshPrep>(e, "mp");
    install_mpjob_one<mpjob::kPrimBuilder>(e, "pb");
    install_mpjob_one<mpjob::kGroupVis>(e, "gv");
    install_mpjob_one<mpjob::kSortPrim>(e, "sp");
}

template <size_t I>
bool ovl_handler(Context& c, Host& h) {
    return edge::ovl::run(c, h, edge::ovl::entry(I));
}

template <size_t... I>
constexpr auto make_ovl_handlers(std::index_sequence<I...>) {
    return std::array<HostHandler, sizeof...(I)>{ovl_handler<I>...};
}

constexpr auto kOvlHandlers = make_ovl_handlers(std::make_index_sequence<128>{});

std::array<Function, 128> g_ovl_funcs{};

void install_ovl() {
    const char* e = std::getenv("GOWA_EDGE_OVL");
    if (e && e[0] == '0') return;
    if (const char* h = std::getenv("GOWA_SPU_HLE"); h && h[0] == '0') return;
    size_t n = edge::ovl::entry_count();
    for (size_t i = 0; i < n && i < g_ovl_funcs.size(); i++) g_ovl_funcs[i] = Function{0, 0, nullptr, nullptr, edge::ovl::entry_name(i), kOvlHandlers[i]};
    g_dyn_on = n && n <= g_ovl_funcs.size();
}

}

bool g_dyn_on = false;

const Function* lookup_dyn(const uint8_t* ls, uint32_t pc) {
    const edge::ovl::Entry* e = edge::ovl::find(ls, pc);
    return e ? &g_ovl_funcs[size_t(edge::ovl::index_of(e))] : nullptr;
}

void install_builtin() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (const char* e = std::getenv("GOWA_TEXCREATOR_HLE"); !e || e[0] != '0') register_function({0x56E0, 32, nullptr, texcreator_compute, "TexCreator.compute", nullptr, kTexCreatorComputeHash});
        install_edge();
        install_ovl();
        install_renjob();
        install_scream();
        install_screamjob();
        install_cloth_shadows();
        install_meshprep();
        install_pm();
        install_edgejob();
        install_wws_job("GOWA_MESHHIER_HLE", "GOWA_MESHHIER_HLE_SKIP", "MeshHier", meshhier::hooks, meshhier::signature, meshhier::run_hook);
        install_wws_job("GOWA_ANIM_HLE", "GOWA_ANIM_HLE_SKIP", "Anim", anim::hooks, anim::signature, anim::run_hook);
        install_tasks();
        install_mpjob();
    });
}

}
