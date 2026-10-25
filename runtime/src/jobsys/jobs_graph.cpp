#include "jobs_graph.h"

#include "ppu/context.h"
#include "runtime/critpath.h"
#include "runtime/jobsys.h"
#include "runtime/log.h"
#include "runtime/memory.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <intrin.h>

namespace wj {

namespace {

constexpr uint32_t kGraph = 0x9aafb0;
constexpr uint32_t kMaxNodes = 8;

struct GraphCfg {
    bool on = false;
    bool loaded = false;
};

GraphCfg& gcfg() {
    static GraphCfg c = [] {
        GraphCfg g;
        const char* e = std::getenv("GOWA_JOBSYS_JOBS");
        g.on = e && e[0] == '1';
        g.loaded = true;
        return g;
    }();
    return c;
}

struct NodeState {
    std::atomic<uint32_t> list{0};
    std::atomic<uint32_t> gate{0};
    jobsys::Completion done;
};

NodeState g_nodes[kMaxNodes];
std::atomic<uint64_t> g_taken{0}, g_left{0}, g_flushes{0}, g_waits{0}, g_wait_cyc{0};
std::atomic<uint64_t> g_seq{1};

int node_of(uint32_t arr, uint32_t& list) {
    uint32_t n = vm::read16(kGraph + 10);
    uint32_t nodes = vm::read32(kGraph + 0x20);
    if (!nodes || n > kMaxNodes || !arr) return -1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t obj = vm::read32(nodes + 0x70 * i + 0x2c);
        uint32_t hdr = obj ? vm::read32(obj) : 0;
        if (!hdr) continue;
        for (uint32_t k = 0; k < 4; k++) {
            if (vm::read32(hdr + 0x30 + 4 * k) == arr) {
                list = obj;
                g_nodes[i].list.store(obj, std::memory_order_relaxed);
                return int(i);
            }
        }
    }
    return -1;
}

bool accept(const jobsys::JobDesc& j) {
    if (!jobsys::module_enabled(j.module_ea)) return false;
    jobsys::Tier t = jobsys::tier_for(j.module_ea);
    if (t == jobsys::Tier::Wws) return false;
    const jobsys::ModuleInfo* m = jobsys::find_module(j.module_ea);
    if (!m) return false;
    if (!j.only_code() && !(m->semantic && t == jobsys::Tier::Semantic)) return false;
    return true;
}

bool on_flush(ppu::Context& ctx) {
    if (!jobsys::enabled()) return false;
    uint32_t b = static_cast<uint32_t>(ctx.gpr[3]);
    uint32_t count = vm::read32(b + 20);
    if (!count) return false;
    uint32_t list = 0;
    int node = node_of(vm::read32(b + 52), list);
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 6)
        LOG_INFO("wj", "graph flush B 0x%08x count %u data 0x%08x ents 0x%08x arr 0x%08x obj 0x%08x -> node %d", b, count, vm::read32(b), vm::read32(b + 48), vm::read32(b + 52), vm::read32(b + 32), node);
    if (node < 0) return false;
    NodeState& ns = g_nodes[node];
    uint32_t data = vm::read32(b + 0);
    uint32_t ents = vm::read32(b + 48);
    std::vector<jobsys::JobDesc> jobs;
    jobs.reserve(count);
    uint64_t now = __rdtsc();
    uint32_t left = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t e = ents + 8 * i;
        uint32_t size = vm::read16(e), type = vm::read16(e + 2), ptr = vm::read32(e + 4);
        if (type == 2) {
            ns.gate.store(ptr, std::memory_order_relaxed);
            continue;
        }
        if (type != 1) continue;
        uint64_t header = (uint64_t(size) << 48) | (uint64_t(1) << 32) | uint32_t(data + ptr);
        jobsys::JobDesc j;
        if (jobsys::decode(header, list, j) != jobsys::Decode::Ok || !accept(j)) {
            left++;
            continue;
        }
        j.list_obj = list;
        j.gate_ea = ns.gate.load(std::memory_order_relaxed);
        j.seq = g_seq.fetch_add(1, std::memory_order_relaxed);
        j.t_submit = now;
        j.submit_lr = 0x44d364;
        jobs.push_back(j);
        vm::write16(e + 2, 3);
    }
    g_flushes.fetch_add(1, std::memory_order_relaxed);
    g_left.fetch_add(left, std::memory_order_relaxed);
    if (jobs.empty()) return false;
    std::vector<jobsys::JobDesc*> ptrs;
    ptrs.reserve(jobs.size());
    for (auto& j : jobs) ptrs.push_back(&j);
    jobsys::submit(ptrs.data(), static_cast<uint32_t>(ptrs.size()), &ns.done);
    g_taken.fetch_add(jobs.size(), std::memory_order_relaxed);
    return false;
}

void drain_all() {
    uint64_t t0 = __rdtsc();
    bool waited = false;
    for (NodeState& ns : g_nodes) {
        if (ns.done.pending.load(std::memory_order_acquire) > 0) {
            waited = true;
            jobsys::help_until(ns.done, 5);
        }
    }
    if (waited) {
        uint64_t t1 = __rdtsc();
        g_waits.fetch_add(1, std::memory_order_relaxed);
        g_wait_cyc.fetch_add(t1 - t0, std::memory_order_relaxed);
        if (critpath::g_on) critpath::emit_wait(critpath::K_GRAPH, t0, t1, kGraph, 0, 0x6f8800);
    }
}

bool on_graph_wait(ppu::Context&) {
    if (!jobsys::enabled()) return false;
    drain_all();
    for (NodeState& ns : g_nodes) ns.gate.store(0, std::memory_order_relaxed);
    return false;
}

bool on_node_wait(ppu::Context& ctx) {
    if (!jobsys::enabled()) return false;
    uint32_t node = static_cast<uint32_t>(ctx.gpr[4]);
    uint32_t nodes = vm::read32(kGraph + 0x20);
    if (nodes && node >= nodes && (node - nodes) % 0x70 == 0 && (node - nodes) / 0x70 < kMaxNodes) {
        NodeState& ns = g_nodes[(node - nodes) / 0x70];
        if (ns.done.pending.load(std::memory_order_acquire) > 0) jobsys::help_until(ns.done, 5);
    } else {
        drain_all();
    }
    return false;
}

void stats_thread() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        uint64_t t = g_taken.exchange(0), l = g_left.exchange(0), f = g_flushes.exchange(0), w = g_waits.exchange(0), wc = g_wait_cyc.exchange(0);
        if (t || l) LOG_INFO("wj", "graph 5s: %llu jobs native, %llu left on WWS, %llu flushes, %llu waits %.2f ms", (unsigned long long)t, (unsigned long long)l, (unsigned long long)f, (unsigned long long)w, double(wc) / 3.0e6);
    }
}

}

void install(void (*hook)(uint32_t addr, bool (*guard)(ppu::Context&))) {
    if (!gcfg().on || !jobsys::enabled()) return;
    hook(0x44d364, on_flush);
    hook(0x44cd20, on_graph_wait);
    hook(0x44d120, on_node_wait);
    std::thread(stats_thread).detach();
    LOG_INFO("wj", "graph list intercept installed (MeshHierarchy, GroupVis, PrimBuilder, Shadows)");
}

}
