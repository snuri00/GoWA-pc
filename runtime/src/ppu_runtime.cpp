#include "ppu/ops.h"
#include "runtime/critpath.h"
#include "runtime/hle.h"
#include "runtime/image.h"
#include "runtime/kernel.h"
#include "runtime/loader.h"
#include "runtime/log.h"
#include "runtime/trace.h"
#include <atomic>
#include "runtime/thread.h"
#include "runtime/memory.h"

#include <algorithm>
#include <chrono>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace lv2 {

namespace {

struct SyscallSlot {
    const char* name = nullptr;
    SyscallFn fn = nullptr;
};

SyscallSlot* table() {
    static SyscallSlot slots[1024];
    return slots;
}

std::atomic<uint8_t> g_warned[1024];

}

SyscallReg::SyscallReg(uint32_t number, const char* name, SyscallFn fn) {
    table()[number & 1023] = {name, fn};
}

const char* syscall_name(uint32_t number) {
    const char* n = table()[number & 1023].name;
    return n ? n : "?";
}

}

namespace hle {

namespace {

std::map<std::string, Fn>& registry() {
    static std::map<std::string, Fn> r;
    return r;
}

}

Reg::Reg(const char* module, const char* name, Fn fn) {
    registry()[std::string(module) + "::" + name] = fn;
}

Fn find(const char* module, const char* name) {
    auto it = registry().find(std::string(module) + "::" + name);
    return it == registry().end() ? nullptr : it->second;
}

}

namespace ppu {

namespace {

struct Range {
    uint32_t lo;
    uint32_t hi;
    std::vector<Func> table;
};

std::vector<Range> g_ranges;
std::once_flag g_dispatch_once;

void build_dispatch() {
    for (uint32_t i = 0; i < image::image_count; i++) {
        g_ranges.push_back({image::images[i].text_lo, image::images[i].text_hi, std::vector<Func>((image::images[i].text_hi - image::images[i].text_lo) / 4)});
    }
    for (uint32_t i = 0; i < image::function_count; i++) {
        uint32_t a = image::functions[i].addr;
        for (auto& r : g_ranges) {
            if (a >= r.lo && a < r.hi) {
                r.table[(a - r.lo) / 4] = image::functions[i].fn;
                break;
            }
        }
    }
}

Func lookup(uint32_t addr) {
    std::call_once(g_dispatch_once, build_dispatch);
    if (addr & 3) return nullptr;
    for (auto& r : g_ranges) {
        if (addr >= r.lo && addr < r.hi) return r.table[(addr - r.lo) / 4];
    }
    return nullptr;
}

enum : int32_t { kUnresolved = -3, kNone = -2, kHle = -1 };

struct ImportSlot {
    std::atomic<int32_t> state{kUnresolved};
    hle::Fn hle = nullptr;
    std::atomic<bool> warned{false};
    bool log_args = false;
};

std::vector<ImportSlot>& import_slots() {
    static std::vector<ImportSlot> v(image::import_count);
    return v;
}

int32_t resolve(uint32_t index, ImportSlot& slot) {
    const image::ImportEntry& imp = image::imports[index];
    if (const char* list = std::getenv("GOWA_IMPORT_ARGS")) {
        std::string l = std::string(",") + list + ",";
        if (imp.name && *imp.name && l.find(std::string(",") + imp.name + ",") != std::string::npos) slot.log_args = true;
    }
    for (uint32_t i = 0; i < image::export_count; i++) {
        const image::ExportEntry& e = image::exports[i];
        if (e.nid == imp.nid && std::strcmp(e.module, imp.module) == 0 && loader::image_loaded(static_cast<int>(e.image))) {
            return static_cast<int32_t>(i);
        }
    }
    if (imp.name && *imp.name) {
        if (hle::Fn fn = hle::find(imp.module, imp.name)) {
            slot.hle = fn;
            return kHle;
        }
    }
    return kNone;
}

}

void call_indirect(Context& ctx, uint8_t* base, uint32_t addr) {
    Func fn = lookup(addr);
    if (!fn) {
        LOG_FATAL("ppu", "unknown indirect target 0x%08x (lr=0x%08llx, thread 0x%x)", addr, static_cast<unsigned long long>(ctx.lr), ctx.thread_id);
    }
    fn(ctx, base);
}

void call_import_body(Context& ctx, uint8_t* base, uint32_t index, ImportSlot& slot, int32_t st, const image::ImportEntry& imp);

void call_import(Context& ctx, uint8_t* base, uint32_t index) {
    ImportSlot& slot = import_slots()[index];
    int32_t st = slot.state.load(std::memory_order_acquire);
    if (st == kUnresolved || st == kNone) {
        st = resolve(index, slot);
        slot.state.store(st, std::memory_order_release);
    }
    const image::ImportEntry& imp = image::imports[index];
    if (trace::on(trace::IMPORT)) {
        static std::atomic<uint32_t> names[16384];
        uint32_t id = names[index & 16383].load(std::memory_order_relaxed);
        if (!id) {
            id = trace::intern(std::string(imp.module) + "::" + (*imp.name ? imp.name : "?"));
            names[index & 16383].store(id, std::memory_order_relaxed);
        }
        uint64_t a0 = ctx.gpr[3], a1 = ctx.gpr[4], a2 = ctx.gpr[5];
        uint32_t lr = static_cast<uint32_t>(ctx.lr);
        uint64_t t0 = trace::now_ns();
        call_import_body(ctx, base, index, slot, st, imp);
        uint64_t us = (trace::now_ns() - t0) / 1000;
        trace::emit(trace::T_IMPORT, id, a0, a1, a2, ctx.gpr[3], (uint64_t(lr) << 32) | (us > 0xFFFFFFFFull ? 0xFFFFFFFFull : us));
        return;
    }
    call_import_body(ctx, base, index, slot, st, imp);
}

void call_import_body(Context& ctx, uint8_t* base, uint32_t index, ImportSlot& slot, int32_t st, const image::ImportEntry& imp) {
    if (slot.log_args) {
        LOG_INFO("import", "%s::%s(0x%llx, 0x%llx, 0x%llx, 0x%llx) lr 0x%llx", imp.module, imp.name, static_cast<unsigned long long>(ctx.gpr[3]),
                 static_cast<unsigned long long>(ctx.gpr[4]), static_cast<unsigned long long>(ctx.gpr[5]), static_cast<unsigned long long>(ctx.gpr[6]),
                 static_cast<unsigned long long>(ctx.lr));
    }
    if (st >= 0) {
        const image::ExportEntry& e = image::exports[st];
        uint32_t sp = static_cast<uint32_t>(ctx.gpr[1]);
        st64(base, sp + 0x28, ctx.gpr[2]);
        uint32_t code = ld32(base, e.opd);
        ctx.gpr[2] = ld32(base, e.opd + 4);
        LOG_TRACE("import", "%s::%s -> LLE 0x%08x", imp.module, imp.name, code);
        call_indirect(ctx, base, code);
        ctx.gpr[2] = ld64(base, sp + 0x28);
        if (slot.log_args) LOG_INFO("import", "%s::%s returned 0x%llx", imp.module, imp.name, static_cast<unsigned long long>(ctx.gpr[3]));
        return;
    }
    if (st == kHle) {
        LOG_TRACE("import", "%s::%s -> HLE", imp.module, imp.name);
        slot.hle(ctx, base);
        return;
    }
    if (!slot.warned.exchange(true)) {
        LOG_WARN("import", "unimplemented %s::%s (0x%08x) r3=0x%llx r4=0x%llx", imp.module, *imp.name ? imp.name : "?", imp.nid,
                 static_cast<unsigned long long>(ctx.gpr[3]), static_cast<unsigned long long>(ctx.gpr[4]));
    }
    ctx.gpr[3] = 0;
}

void syscall(Context& ctx, uint8_t* base) {
    uint32_t n = static_cast<uint32_t>(ctx.gpr[11]) & 1023;
    auto& slot = lv2::table()[n];
    if (slot.fn) {
        LOG_TRACE("lv2", "%s(0x%llx, 0x%llx, 0x%llx, 0x%llx)", slot.name, static_cast<unsigned long long>(ctx.gpr[3]), static_cast<unsigned long long>(ctx.gpr[4]),
                  static_cast<unsigned long long>(ctx.gpr[5]), static_cast<unsigned long long>(ctx.gpr[6]));
        uint64_t cp0 = critpath::g_on ? __rdtsc() : 0;
        guest::Thread* t = guest::current();
        if (t) {
            t->in_syscall.store(n + 1, std::memory_order_relaxed);
            t->syscalls.fetch_add(1, std::memory_order_relaxed);
            t->syscall_hist[n]++;
        }
        if (trace::on(trace::SYSCALL)) {
            uint64_t a0 = ctx.gpr[3], a1 = ctx.gpr[4], a2 = ctx.gpr[5];
            uint32_t lr = static_cast<uint32_t>(ctx.lr);
            uint64_t t0 = trace::now_ns();
            ctx.gpr[3] = static_cast<uint64_t>(slot.fn(ctx, base));
            uint64_t us = (trace::now_ns() - t0) / 1000;
            trace::emit(trace::T_SYSCALL, n, a0, a1, a2, ctx.gpr[3], (uint64_t(lr) << 32) | (us > 0xFFFFFFFFull ? 0xFFFFFFFFull : us));
        } else {
            ctx.gpr[3] = static_cast<uint64_t>(slot.fn(ctx, base));
        }
        if (t) t->in_syscall.store(0, std::memory_order_relaxed);
        if (cp0) critpath::syscall_done(n, cp0, static_cast<uint32_t>(ctx.lr));
        return;
    }
    if (!lv2::g_warned[n].exchange(1)) {
        LOG_WARN("lv2", "unimplemented syscall %u at 0x%08x r3=0x%llx r4=0x%llx r5=0x%llx", n, ctx.cia,
                 static_cast<unsigned long long>(ctx.gpr[3]), static_cast<unsigned long long>(ctx.gpr[4]), static_cast<unsigned long long>(ctx.gpr[5]));
    }
    ctx.gpr[3] = 0;
}

}
