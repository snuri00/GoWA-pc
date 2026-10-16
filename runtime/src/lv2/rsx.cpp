#include "runtime/hostsleep.h"
#include "event.h"
#include "gpu/presenter.h"
#include "gpu/rsx.h"
#include "ppu/ops.h"
#include "runtime/reservation.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace crash {
std::string sample_stack(void* native, int depth);
void profile_thread(void* native, const char* name);
}

namespace lv2 {

namespace {

constexpr uint32_t CONTEXT_ID = 0x55555555;
constexpr uint32_t LOCAL_BASE = 0xC0000000;

struct RsxState {
    std::mutex m;
    uint32_t local_size = 0;
    uint32_t device_addr = 0;
    uint32_t ctx_base = 0;
    uint32_t dma_control = 0;
    uint32_t driver_info = 0;
    uint32_t reports = 0;
    uint32_t queue_id = 0;
    uint32_t port_id = 0;
    uint32_t io_table[512] = {};
    std::atomic<bool> running{false};
    uint32_t sema_ctx_406e = 0;
    uint32_t sema_off_406e = 0;
    uint32_t sema_ctx_4097 = 0;
    uint32_t sema_off_4097 = 0;
    uint32_t report_ctx = 0;
    bool call_active = false;
    uint32_t call_return = 0;
    uint64_t flips = 0;
    std::atomic<uint32_t> last_method{0};
    std::atomic<uint32_t> last_arg{0};
    std::atomic<uint32_t> waiting_addr{0};
    std::atomic<uint32_t> waiting_value{0};
    std::atomic<int64_t> pending_flip{-1};
};

RsxState g_rsx;

struct DummyPort : Object {};

uint32_t head_addr(uint32_t head) { return g_rsx.driver_info + 0x10B8 + head * 0x40; }

void send_event(uint64_t bits) {
    uint32_t handlers = vm::read32(g_rsx.driver_info + 0x12C0);
    if (!(bits >> 32) && !(handlers & bits)) return;
    auto q = get<EventQueue>(g_rsx.queue_id);
    if (!q) return;
    for (int i = 0; i < 100 && push_event(q, {(uint64_t(1) << 32) | g_rsx.port_id, 0, bits, 0}) == CELL_EBUSY; i++) Sleep(1);
}

uint32_t io_to_ea(uint32_t io) {
    uint32_t page = g_rsx.io_table[(io >> 20) & 511];
    if (!page) return 0;
    return (page << 20) | (io & 0xFFFFF);
}

uint32_t get_address(uint32_t offset, uint32_t loc) {
    switch (loc) {
    case 0:
    case 0xFEED0000: return LOCAL_BASE + offset;
    case 1:
    case 0xFEED0001: return io_to_ea(offset);
    case 0x66626660: return g_rsx.reports + 0x1400 + offset;
    case 0xBAD68000: return io_to_ea(0x0E000000 + offset);
    case 0x66606660:
    case 0x66616661: return g_rsx.reports + offset;
    case 0x56616660:
    case 0x56616661: return g_rsx.device_addr + offset;
    default:
        LOG_WARN("rsx", "unknown DMA context 0x%08x (offset 0x%x)", loc, offset);
        return 0;
    }
}

void rsx_write32(uint32_t addr, uint32_t value) {
    uint32_t be = __builtin_bswap32(value);
    rsv::store_locked(addr, &be, 4);
}

void flip_done(uint32_t buffer) {
    uint32_t h = head_addr(1);
    vm::write32(h + 8, vm::read32(h + 8) | 0x80000000u);
    vm::write64(h, ppu::timebase());
    vm::write32(h + 0x10, buffer);
    std::memset(vm::ptr(g_rsx.reports + 0x10), 0, 16);
    g_rsx.flips++;
    if ((g_rsx.flips % 60) == 1) LOG_INFO("rsx", "flip %llu (buffer %u)", static_cast<unsigned long long>(g_rsx.flips), buffer);
    send_event(1u << 4);
}

int64_t context_attribute(uint32_t package, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    switch (package) {
    case 0x001:
        vm::write32(g_rsx.dma_control + 0x44, static_cast<uint32_t>(a3));
        vm::write32(g_rsx.dma_control + 0x40, static_cast<uint32_t>(a4));
        return CELL_OK;
    case 0x102: {
        uint32_t head = static_cast<uint32_t>(a3);
        uint32_t v = static_cast<uint32_t>(a4);
        uint32_t buffer = (v & 0x80000000u) ? (v & 0xF) : 0;
        if ((v & 0x80000000u) && buffer > 7) buffer = vm::read32(head_addr(head) + 0x14);
        if (!gpu::rsx::present(buffer)) gpu::flip(buffer);
        g_rsx.pending_flip = buffer;
        while (g_rsx.pending_flip >= 0 && g_rsx.running) Sleep(0);
        return CELL_OK;
    }
    case 0x103: {
        uint32_t head = static_cast<uint32_t>(a3) & 1;
        uint32_t id = static_cast<uint32_t>(a4);
        uint32_t h = head_addr(head);
        vm::write32(h + 0x14, id);
        vm::write32(h + 8, vm::read32(h + 8) | 0x40000000u | (1u << (id & 7)));
        send_event(1u << (5 + head));
        return CELL_OK;
    }
    case 0x10A: {
        uint32_t head = static_cast<uint32_t>(a3);
        if (head > 7) return -17;
        uint32_t h = head_addr(head);
        vm::write32(h + 8, (vm::read32(h + 8) & static_cast<uint32_t>(a4)) | static_cast<uint32_t>(a5));
        return CELL_OK;
    }
    case 0xFEF:
        if (static_cast<uint32_t>(a4) != vm::read32(g_rsx.driver_info + 0x12CC)) {
            vm::write32(g_rsx.driver_info + 0x12CC, static_cast<uint32_t>(a4));
        }
        send_event(1u << 7);
        return CELL_OK;
    case 0x104:
        gpu::set_display_buffer(static_cast<uint32_t>(a3) & 0xFF, static_cast<uint32_t>(a5), static_cast<uint32_t>(a5 >> 32), static_cast<uint32_t>(a4 >> 32),
                                static_cast<uint32_t>(a4));
        return CELL_OK;
    case 0x100: case 0x101: case 0x105: case 0x106: case 0x108: case 0x10B: case 0x10C: case 0x10D:
    case 0x300: case 0x301: case 0x302: case 0x600: case 0x601: case 0x602: case 0x603:
        return CELL_OK;
    default:
        LOG_WARN("rsx", "unknown context attribute package 0x%x", package);
        return CELL_EINVAL;
    }
}

void semaphore_wait(uint32_t addr, uint32_t value) {
    static const uint64_t tdr_us = std::getenv("GOWA_TDR_US") ? std::strtoull(std::getenv("GOWA_TDR_US"), nullptr, 10) : 1000000;
    g_rsx.waiting_addr = addr;
    g_rsx.waiting_value = value;
    auto start = std::chrono::steady_clock::now();
    for (int spins = 0; g_rsx.running; spins++) {
        if (vm::read32(addr) == value) {
            g_rsx.waiting_addr = 0;
            return;
        }
        if (spins < 64) {
            std::this_thread::yield();
            continue;
        }
        Sleep(0);
        if (tdr_us && (spins & 255) == 0) {
            auto waited = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
            if (static_cast<uint64_t>(waited) > tdr_us) {
                LOG_ERROR("rsx", "semaphore acquire timed out: 0x%08x == 0x%x (value 0x%x)", addr, value, vm::read32(addr));
                g_rsx.waiting_addr = 0;
                return;
            }
        }
    }
}

std::atomic<uint64_t> g_method_counts[0x4000];

void execute(uint32_t reg, uint32_t arg) {
    g_method_counts[(reg >> 2) & 0x3FFF].fetch_add(1, std::memory_order_relaxed);
    g_rsx.last_method.store(reg, std::memory_order_relaxed);
    g_rsx.last_arg.store(arg, std::memory_order_relaxed);
    if (reg >= 0xE000) LOG_DEBUG("rsx", "driver method 0x%04x arg 0x%08x", reg, arg);
    static const bool trace = std::getenv("GOWA_RSX_TRACE") != nullptr;
    if (trace) LOG_INFO("rsxm", "0x%04x 0x%08x", reg, arg);
    if (reg < 0xE000) gpu::rsx::method(reg, arg);
    switch (reg) {
    case 0x50: vm::write32(g_rsx.dma_control + 0x48, arg); break;
    case 0x60: g_rsx.sema_ctx_406e = arg; break;
    case 0x64: g_rsx.sema_off_406e = arg; break;
    case 0x68:
        LOG_DEBUG("rsx", "acquire ctx 0x%08x off 0x%x == 0x%x", g_rsx.sema_ctx_406e, g_rsx.sema_off_406e, arg);
        if (uint32_t a = get_address(g_rsx.sema_off_406e, g_rsx.sema_ctx_406e)) semaphore_wait(a, arg);
        break;
    case 0x6C:
        LOG_DEBUG("rsx", "release ctx 0x%08x off 0x%x = 0x%x", g_rsx.sema_ctx_406e, g_rsx.sema_off_406e, arg);
        if (uint32_t a = get_address(g_rsx.sema_off_406e, g_rsx.sema_ctx_406e)) {
            if (a == g_rsx.device_addr + 0x30 && !arg) arg = 1;
            rsx_write32(a, arg);
        }
        break;
    case 0x1A4: g_rsx.sema_ctx_4097 = arg; break;
    case 0x1A8: g_rsx.report_ctx = arg; break;
    case 0x1D6C: g_rsx.sema_off_4097 = arg; break;
    case 0x1D70:
        LOG_DEBUG("rsx", "backend release ctx 0x%08x off 0x%x = 0x%x", g_rsx.sema_ctx_4097, g_rsx.sema_off_4097, arg);
        if (uint32_t a = get_address(g_rsx.sema_off_4097, g_rsx.sema_ctx_4097)) {
            rsx_write32(a, (arg & 0xFF00FF00u) | ((arg & 0xFF) << 16) | ((arg >> 16) & 0xFF));
        }
        break;
    case 0x1D74:
        LOG_DEBUG("rsx", "texture release ctx 0x%08x off 0x%x = 0x%x", g_rsx.sema_ctx_4097, g_rsx.sema_off_4097, arg);
        if (uint32_t a = get_address(g_rsx.sema_off_4097, g_rsx.sema_ctx_4097)) rsx_write32(a, arg);
        break;
    case 0x1800: {
        uint32_t offset = arg & 0xFFFFFF;
        uint32_t loc = g_rsx.report_ctx ? g_rsx.report_ctx : 0x66626660;
        if (uint32_t a = get_address(offset, loc)) {
            uint8_t rep[16];
            uint64_t t = __builtin_bswap64(ppu::timebase());
            std::memcpy(rep, &t, 8);
            std::memset(rep + 8, 0, 8);
            rsv::store_locked(a, rep, 16);
            LOG_DEBUG("rsx", "report type %u loc 0x%08x off 0x%x -> ea 0x%08x", arg >> 24, loc, offset, a);
        }
        break;
    }
    case 0xE920: context_attribute(0x102, 0, arg, 0, 0); break;
    case 0xE924: context_attribute(0x102, 1, arg, 0, 0); break;
    case 0xEB00: context_attribute(0xFEF, 0, arg, 0, 0); break;
    default:
        if (reg >= 0xE940 && reg < 0xE960) context_attribute(0x103, (reg - 0xE940) / 4, arg, 0, 0);
        break;
    }
}

HANDLE g_fifo_handle = nullptr;

struct LineReader {
    uint32_t line = ~0u;
    alignas(16) uint8_t buf[128];
    uint32_t read(uint32_t ea) {
        uint32_t l = ea & ~127u;
        if (l != line) {
            rsv::snapshot(l, buf);
            line = l;
        }
        uint32_t v;
        std::memcpy(&v, buf + (ea & 127u), 4);
        return __builtin_bswap32(v);
    }
};

void fifo_thread() {
    SetThreadDescription(GetCurrentThread(), L"RSX FIFO");
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_fifo_handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
    if (std::getenv("GOWA_PROFILE_FIFO")) crash::profile_thread(g_fifo_handle, "rsx fifo");
    LOG_INFO("rsx", "null RSX FIFO thread started");
    uint32_t ctrl = g_rsx.dma_control;
    uint32_t idle = 0;
    while (g_rsx.running) {
        uint32_t put = vm::read32(ctrl + 0x40);
        uint32_t get = vm::read32(ctrl + 0x44);
        if (get == put) {
            if (++idle < 256) std::this_thread::yield();
            else Sleep(1);
            continue;
        }
        idle = 0;
        static uint32_t hist_get[64], hist_cmd[64], hist_put[64];
        static uint32_t hist_n = 0;
        uint32_t ea = io_to_ea(get);
        if (!ea) {
            static uint32_t dumped = 0xFFFFFFFF;
            if (dumped != get) {
                dumped = get;
                for (uint32_t i = 0; i < 64; i++) {
                    uint32_t k = (hist_n + i) & 63;
                    LOG_ERROR("rsx", "  hist get 0x%08x cmd 0x%08x put 0x%08x", hist_get[k], hist_cmd[k], hist_put[k]);
                }
                for (uint32_t h = 1; h <= 4; h++) {
                uint32_t lastget = hist_get[(hist_n - h) & 63];
                uint32_t lea = io_to_ea(lastget & ~0x3Fu);
                if (lea) {
                    for (int r = -4; r < 4; r++) {
                        uint32_t a = lea + r * 32;
                        LOG_ERROR("rsx", "  mem io 0x%08x: %08x %08x %08x %08x %08x %08x %08x %08x", (lastget & ~0x3Fu) + r * 32, vm::read32(a), vm::read32(a + 4), vm::read32(a + 8), vm::read32(a + 12),
                                  vm::read32(a + 16), vm::read32(a + 20), vm::read32(a + 24), vm::read32(a + 28));
                    }
                }
                }
            }
            LOG_ERROR("rsx", "FIFO get 0x%08x not IO-mapped (put 0x%08x)", get, put);
            Sleep(10);
            continue;
        }
        LineReader reader;
        uint32_t cmd = reader.read(ea);
        if (cmd) {
            hist_get[hist_n & 63] = get;
            hist_cmd[hist_n & 63] = cmd;
            hist_put[hist_n & 63] = put;
            hist_n++;
        }
        static const bool trace = std::getenv("GOWA_RSX_TRACE") != nullptr;
        if (trace) LOG_INFO("rsxc", "get 0x%08x cmd 0x%08x", get, cmd);
        if ((cmd & 0xE0000003) == 0x20000000 || (cmd & 0xE0000003) == 0x00000001) {
            uint32_t target = (cmd & 3) == 1 ? cmd & 0xFFFFFFFC : cmd & 0x1FFFFFFC;
            if (target == get) {
                if (++idle < 20000) std::this_thread::yield();
                else Sleep(1);
                continue;
            }
            vm::write32(ctrl + 0x44, target);
            continue;
        }
        if ((cmd & 3) == 2) {
            if (g_rsx.call_active) LOG_WARN("rsx", "nested FIFO call at 0x%08x (return 0x%08x)", get, g_rsx.call_return);
            g_rsx.call_active = true;
            g_rsx.call_return = get + 4;
            vm::write32(ctrl + 0x44, cmd & 0x1FFFFFFC);
            continue;
        }
        if ((cmd & 0xFFFF0003) == 0x00020000) {
            if (!g_rsx.call_active) {
                LOG_ERROR("rsx", "FIFO return without call at 0x%08x", get);
                for (uint32_t i = 0; i < 64; i++) {
                    uint32_t k = (hist_n + i) & 63;
                    LOG_ERROR("rsx", "  hist get 0x%08x cmd 0x%08x put 0x%08x", hist_get[k], hist_cmd[k], hist_put[k]);
                }
                vm::write32(ctrl + 0x44, get + 4);
                continue;
            }
            g_rsx.call_active = false;
            vm::write32(ctrl + 0x44, g_rsx.call_return);
            continue;
        }
        uint32_t count = (cmd >> 18) & 0x7FF;
        uint32_t method = cmd & 0xFFFC;
        bool noninc = (cmd & 0xE0030003) == 0x40000000;
        for (uint32_t i = 0; i < count; i++) {
            execute(noninc ? method : method + i * 4, reader.read(ea + 4 + i * 4));
        }
        vm::write32(ctrl + 0x44, get + 4 + count * 4);
    }
}

void vblank_thread() {
    SetThreadDescription(GetCurrentThread(), L"RSX VBlank");
    auto next = std::chrono::steady_clock::now();
    while (g_rsx.running) {
        next += std::chrono::nanoseconds(16683333);
        host::sleep_until_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(next.time_since_epoch()).count());
        uint32_t h = head_addr(1);
        vm::write64(h + 0x30, vm::read64(h + 0x30) + 1);
        uint64_t tb = ppu::timebase();
        vm::write32(h + 0x1C, static_cast<uint32_t>(tb));
        vm::write32(h + 0x3C, static_cast<uint32_t>(tb >> 32));
        int64_t pf = g_rsx.pending_flip.exchange(-1);
        if (pf >= 0) flip_done(static_cast<uint32_t>(pf));
        send_event(1u << 1);
    }
}

}

uint32_t rsx_address(uint32_t offset, uint32_t loc) { return get_address(offset, loc); }

void rsx_status() {
    if (!g_rsx.dma_control) return;
    if (g_fifo_handle) LOG_INFO("status", "rsx fifo stack: %s", crash::sample_stack(g_fifo_handle, 16).c_str());
    if (std::getenv("GOWA_RSX_STATS")) {
        std::string line;
        char b[32];
        int n = 0;
        for (uint32_t i = 0; i < 0x4000; i++) {
            uint64_t c = g_method_counts[i].load();
            if (!c) continue;
            std::snprintf(b, sizeof b, " %04x:%llu", i * 4, static_cast<unsigned long long>(c));
            line += b;
            if (++n % 16 == 0) {
                LOG_INFO("rsxstat", "%s", line.c_str());
                line.clear();
            }
        }
        if (!line.empty()) LOG_INFO("rsxstat", "%s", line.c_str());
    }
    uint32_t put = vm::read32(g_rsx.dma_control + 0x40), get = vm::read32(g_rsx.dma_control + 0x44);
    LOG_INFO("status", "rsx handlers 0x%08x head1 flags 0x%08x vblanks %llu", vm::read32(g_rsx.driver_info + 0x12C0), vm::read32(head_addr(1) + 8),
             static_cast<unsigned long long>(vm::read64(head_addr(1) + 0x30)));
    LOG_INFO("status", "rsx put 0x%08x get 0x%08x ref 0x%08x flips %llu last method 0x%04x arg 0x%08x wait 0x%08x==0x%08x (now 0x%08x)", put, get,
             vm::read32(g_rsx.dma_control + 0x48), static_cast<unsigned long long>(g_rsx.flips), g_rsx.last_method.load(), g_rsx.last_arg.load(),
             g_rsx.waiting_addr.load(), g_rsx.waiting_value.load(), g_rsx.waiting_addr ? vm::read32(g_rsx.waiting_addr) : 0);
}

LV2_SYSCALL(666, sys_rsx_device_open) { return CELL_OK; }
LV2_SYSCALL(667, sys_rsx_device_close) { return CELL_OK; }

LV2_SYSCALL(675, sys_rsx_device_map) {
    std::lock_guard lk(g_rsx.m);
    if (!g_rsx.device_addr) {
        g_rsx.device_addr = vm::alloc(vm::Region::RsxContext, 0x100000, 0x100000);
        if (!g_rsx.device_addr) return CELL_ENOMEM;
    }
    vm::write64(u32arg(ctx, 0), g_rsx.device_addr);
    return CELL_OK;
}

LV2_SYSCALL(676, sys_rsx_device_unmap) { return CELL_OK; }

LV2_SYSCALL(668, sys_rsx_memory_allocate) {
    uint64_t size = u64arg(ctx, 2);
    if (!size || size % 0x100000 || size > 0x10000000) return -17;
    if (!vm::map(LOCAL_BASE, static_cast<uint32_t>(size))) return CELL_ENOMEM;
    g_rsx.local_size = static_cast<uint32_t>(size);
    vm::write32(u32arg(ctx, 0), 0x5A5A5A5B);
    vm::write64(u32arg(ctx, 1), LOCAL_BASE);
    LOG_INFO("rsx", "local memory 0x%llx bytes at 0x%08x", static_cast<unsigned long long>(size), LOCAL_BASE);
    return CELL_OK;
}

LV2_SYSCALL(669, sys_rsx_memory_free) { return CELL_OK; }

LV2_SYSCALL(670, sys_rsx_context_allocate) {
    if (!g_rsx.local_size) return CELL_EINVAL;
    uint32_t area = vm::alloc(vm::Region::RsxContext, 0x300000, 0x100000);
    if (!area) return CELL_ENOMEM;
    g_rsx.ctx_base = area;
    g_rsx.dma_control = area;
    g_rsx.driver_info = area + 0x100000;
    g_rsx.reports = area + 0x200000;
    uint32_t r = g_rsx.reports;
    static const uint32_t pattern[4] = {0x1337C0D3, 0x1337BABE, 0x1337BEEF, 0x1337F001};
    for (uint32_t i = 0; i < 1024; i++) vm::write32(r + i * 4, pattern[i & 3]);
    for (uint32_t i = 0; i < 64; i++) {
        vm::write64(r + 0x1000 + i * 16, ~0ull);
        vm::write64(r + 0x1000 + i * 16 + 8, 0);
    }
    for (uint32_t i = 0; i < 2048; i++) {
        vm::write64(r + 0x1400 + i * 16, ~0ull);
        vm::write32(r + 0x1400 + i * 16 + 8, 0);
        vm::write32(r + 0x1400 + i * 16 + 12, 0xFFFFFFFF);
    }
    uint32_t d = g_rsx.driver_info;
    std::memset(vm::ptr(d), 0, 0x12F8);
    vm::write32(d + 0x00, 0x211);
    vm::write32(d + 0x04, 0x5C);
    vm::write32(d + 0x08, g_rsx.local_size);
    vm::write32(d + 0x0C, 1);
    vm::write32(d + 0x10, 500000000);
    vm::write32(d + 0x14, 650000000);
    vm::write32(d + 0x2C, 0x1000);
    vm::write32(d + 0x30, 0);
    vm::write32(d + 0x34, 0x1400);
    vm::write32(d + 0x54, static_cast<uint32_t>(u64arg(ctx, 5)));
    auto q = std::make_shared<EventQueue>();
    q->size = 0x20;
    q->name = "_gcmQ";
    g_rsx.queue_id = add_object(0x8D000000, q);
    g_rsx.port_id = add_object(0x0E000000, std::make_shared<DummyPort>());
    vm::write32(d + 0x12D0, g_rsx.queue_id);
    std::memset(vm::ptr(area), 0, 0x100);
    if (g_rsx.device_addr) vm::write32(g_rsx.device_addr + 0x30, 1);
    vm::write32(u32arg(ctx, 0), CONTEXT_ID);
    vm::write64(u32arg(ctx, 1), g_rsx.dma_control);
    vm::write64(u32arg(ctx, 2), g_rsx.driver_info);
    vm::write64(u32arg(ctx, 3), g_rsx.reports);
    if (!g_rsx.running.exchange(true)) {
        std::thread(fifo_thread).detach();
        std::thread(vblank_thread).detach();
    }
    LOG_INFO("rsx", "context at 0x%08x (driver 0x%08x reports 0x%08x queue 0x%x)", area, g_rsx.driver_info, g_rsx.reports, g_rsx.queue_id);
    return CELL_OK;
}

LV2_SYSCALL(671, sys_rsx_context_free) {
    if (u32arg(ctx, 0) != CONTEXT_ID) return CELL_EINVAL;
    return CELL_OK;
}

LV2_SYSCALL(672, sys_rsx_context_iomap) {
    if (u32arg(ctx, 0) != CONTEXT_ID) return CELL_EINVAL;
    uint32_t io = u32arg(ctx, 1), ea = u32arg(ctx, 2), size = u32arg(ctx, 3);
    if ((io | ea | size) & 0xFFFFF || !size) return CELL_EINVAL;
    for (uint32_t off = 0; off < size; off += 0x100000) g_rsx.io_table[((io + off) >> 20) & 511] = (ea + off) >> 20;
    LOG_INFO("rsx", "iomap io 0x%08x -> ea 0x%08x size 0x%x", io, ea, size);
    return CELL_OK;
}

LV2_SYSCALL(673, sys_rsx_context_iounmap) {
    uint32_t io = u32arg(ctx, 1), size = u32arg(ctx, 2);
    for (uint32_t off = 0; off < size; off += 0x100000) g_rsx.io_table[((io + off) >> 20) & 511] = 0;
    return CELL_OK;
}

LV2_SYSCALL(674, sys_rsx_context_attribute) {
    if (u32arg(ctx, 0) != CONTEXT_ID || !g_rsx.driver_info) return CELL_EINVAL;
    return context_attribute(u32arg(ctx, 1), u64arg(ctx, 2), u64arg(ctx, 3), u64arg(ctx, 4), u64arg(ctx, 5));
}

LV2_SYSCALL(677, sys_rsx_attribute) { return CELL_OK; }

}
