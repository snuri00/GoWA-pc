#include "common.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace hle {

namespace {

struct Port {
    bool open = false;
    bool running = false;
    uint32_t channels = 0;
    uint32_t blocks = 0;
    uint32_t addr = 0;
    uint32_t size = 0;
    uint64_t index = 0;
};

std::mutex g_lock;
Port g_ports[8];
uint32_t g_index_area = 0;
std::atomic<bool> g_init{false};

void audio_thread() {
    SetThreadDescription(GetCurrentThread(), L"cellAudio");
    auto next = std::chrono::steady_clock::now();
    const auto period = std::chrono::microseconds(5333);
    while (g_init) {
        next += period;
        std::this_thread::sleep_until(next);
        std::lock_guard lk(g_lock);
        for (uint32_t i = 0; i < 8; i++) {
            Port& p = g_ports[i];
            if (!p.open || !p.running || !p.blocks) continue;
            p.index = (p.index + 1) % p.blocks;
            vm::write64(g_index_area + i * 8, p.index);
        }
    }
}

}

HLE_FUNC(cellAudio, cellAudioInit) {
    if (g_init.exchange(true)) return ret(ctx, static_cast<int32_t>(0x80310701));
    g_index_area = vm::alloc(vm::Region::User64K, 0x10000, 0x10000);
    std::thread(audio_thread).detach();
    LOG_INFO("cellAudio", "cellAudioInit");
    ret(ctx, 0);
}

HLE_FUNC(cellAudio, cellAudioQuit) {
    g_init = false;
    ret(ctx, 0);
}

HLE_FUNC(cellAudio, cellAudioPortOpen) {
    uint32_t param = a32(ctx, 0), out = a32(ctx, 1);
    if (!g_init) return ret(ctx, static_cast<int32_t>(0x80310702));
    if (!param || !out) return ret(ctx, static_cast<int32_t>(0x80310703));
    uint64_t ch = vm::read64(param), blocks = vm::read64(param + 8);
    if (!(ch == 1 || ch == 2 || ch == 6 || ch == 8) || !(blocks == 2 || blocks == 4 || blocks == 8 || blocks == 16 || blocks == 32)) {
        return ret(ctx, static_cast<int32_t>(0x80310703));
    }
    std::lock_guard lk(g_lock);
    for (uint32_t i = 0; i < 8; i++) {
        Port& p = g_ports[i];
        if (p.open) continue;
        p = Port{};
        p.open = true;
        p.channels = static_cast<uint32_t>(ch);
        p.blocks = static_cast<uint32_t>(blocks);
        p.size = p.channels * p.blocks * 256 * 4;
        p.addr = vm::alloc(vm::Region::User64K, p.size, 0x10000);
        vm::write64(g_index_area + i * 8, 0);
        vm::write32(out, i);
        LOG_INFO("cellAudio", "port %u open: %u ch, %u blocks at 0x%08x", i, p.channels, p.blocks, p.addr);
        return ret(ctx, 0);
    }
    ret(ctx, static_cast<int32_t>(0x80310705));
}

HLE_FUNC(cellAudio, cellAudioGetPortConfig) {
    uint32_t n = a32(ctx, 0), out = a32(ctx, 1);
    if (n >= 8 || !out) return ret(ctx, static_cast<int32_t>(0x80310703));
    std::lock_guard lk(g_lock);
    Port& p = g_ports[n];
    vm::write32(out, g_index_area + n * 8);
    vm::write32(out + 4, !p.open ? 0x1010 : p.running ? 2 : 1);
    vm::write64(out + 8, p.channels);
    vm::write64(out + 16, p.blocks);
    vm::write32(out + 24, p.size);
    vm::write32(out + 28, p.addr);
    ret(ctx, 0);
}

HLE_FUNC(cellAudio, cellAudioPortStart) {
    uint32_t n = a32(ctx, 0);
    if (n >= 8) return ret(ctx, static_cast<int32_t>(0x80310703));
    std::lock_guard lk(g_lock);
    if (!g_ports[n].open) return ret(ctx, static_cast<int32_t>(0x80310704));
    g_ports[n].running = true;
    ret(ctx, 0);
}

HLE_FUNC(cellAudio, cellAudioPortStop) {
    uint32_t n = a32(ctx, 0);
    if (n >= 8) return ret(ctx, static_cast<int32_t>(0x80310703));
    std::lock_guard lk(g_lock);
    g_ports[n].running = false;
    ret(ctx, 0);
}

HLE_FUNC(cellAudio, cellAudioPortClose) {
    uint32_t n = a32(ctx, 0);
    if (n >= 8) return ret(ctx, static_cast<int32_t>(0x80310703));
    std::lock_guard lk(g_lock);
    if (g_ports[n].addr) vm::dealloc(g_ports[n].addr);
    g_ports[n] = Port{};
    ret(ctx, 0);
}

}
