#include "ppu/ops.h"
#include "runtime/hooks.h"
#include "runtime/hostsleep.h"
#include "runtime/log.h"
#include "runtime/memory.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace hooks {

namespace {

constexpr uint32_t FN_TIMESTEP = 0x2d0258;
constexpr uint32_t TIMESTEP_POOL = 0x2d0240;
constexpr uint32_t REFRESH_HZ = 0x83b45c;
constexpr uint32_t TIME_FLAGS = 0x83b460;
constexpr uint32_t SLOWMO_B = 0x83b464;
constexpr uint32_t SLOWMO_C = 0x83b468;
constexpr uint32_t SLOWMO_A = 0x83b470;
constexpr uint32_t SCALE_ALL = 0x83b474;
constexpr uint32_t SCALE_CHANNEL = 0x83b478;
constexpr uint32_t REFRESH_INT = 0x9424a0;
constexpr uint32_t DT_RAW = 0x9424a4;
constexpr uint32_t SMOOTH_MODE = 0x9424a8;
constexpr uint32_t DT_CHANNEL = 0x9424ac;
constexpr uint32_t ELAPSED_TB = 0x9424c8;
constexpr uint32_t LAST_TB = 0x9424d0;
constexpr uint32_t FIXED_STEP = 0x9424d8;
constexpr uint32_t TB_FREQ_CACHE = 0x8583e0;

constexpr uint64_t TIMESTEP_CODE_HASH = 0x741ba204cc7c2c0dull;
constexpr uint64_t TIMESTEP_CONSTS_HASH = 0xb75067a16cf2c66aull;
constexpr uint64_t TIMESTEP_LOADS_HASH = 0xf929c0f4bfa179b5ull;

struct Config {
    bool on = false;
    bool follow_refresh = false;
    double fps = 0.0;
};

const Config& config() {
    static const Config c = [] {
        Config c;
        const char* e = std::getenv("GOWA_FPS_UNLOCK");
        double v = (e && *e) ? std::atof(e) : 1.0;
        if (v <= 0.0) return c;
        c.on = true;
        if (v < 1.5) {
            c.follow_refresh = true;
            return c;
        }
        c.fps = std::clamp(v, 10.0, 1000.0);
        return c;
    }();
    return c;
}

float f32(uint32_t ea) { return std::bit_cast<float>(vm::read32(ea)); }
void put_f32(uint32_t ea, float v) { vm::write32(ea, std::bit_cast<uint32_t>(v)); }

bool words_match(uint32_t ea, uint64_t expect, size_t n) {
    if (!vm::range_mapped(ea, n * 4)) return false;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) {
        uint32_t w = vm::read32(ea + static_cast<uint32_t>(i) * 4);
        for (int k = 3; k >= 0; k--) h = (h ^ ((w >> (8 * k)) & 0xffu)) * 0x100000001b3ull;
    }
    return h == expect;
}

int g_state = 0;

bool verified() {
    if (g_state) return g_state > 0;
    bool ok = words_match(FN_TIMESTEP, TIMESTEP_CODE_HASH, 8) && words_match(TIMESTEP_POOL, TIMESTEP_CONSTS_HASH, 6) && words_match(0x2d02d0, TIMESTEP_LOADS_HASH, 8) &&
              vm::range_mapped(REFRESH_HZ, 0x40) && vm::range_mapped(REFRESH_INT, 0x40) && vm::range_mapped(TB_FREQ_CACHE, 8);
    g_state = ok ? 1 : -1;
    const Config& c = config();
    if (!ok) {
        LOG_WARN("fps", "fps unlock timestep guard disabled: timestep code at 0x%08x does not match v1.12", FN_TIMESTEP);
        return false;
    }
    if (c.follow_refresh) LOG_INFO("fps", "immediate flips, frame limiter at display refresh, game timestep unchanged");
    else LOG_INFO("fps", "fps unlock: frame limiter at %.2f fps, timestep floor 1/%.2f s", c.fps, c.fps);
    return true;
}

bool refresh_sane(float hz) { return hz > 20.0f && hz < 130.0f; }

uint64_t tb_freq() {
    uint64_t f = vm::read64(TB_FREQ_CACHE);
    return f ? f : 80000000ull;
}

void pace(double fps) {
    static uint64_t next = 0;
    uint64_t freq = tb_freq();
    uint64_t period = static_cast<uint64_t>(static_cast<double>(freq) / fps);
    uint64_t now = ppu::timebase();
    if (!next || now >= next + period || next > now + period * 2) {
        next = now + period;
        return;
    }
    if (now < next) {
        double us = static_cast<double>(next - now) * 1e6 / static_cast<double>(freq) / ppu::time_scale();
        if (us > 1500.0) host::sleep_us(static_cast<uint64_t>(us - 1000.0));
        while (ppu::timebase() < next) std::this_thread::yield();
    }
    next += period;
}

void write_dt(float dt) {
    put_f32(DT_RAW, dt);
    float all = f32(SCALE_ALL);
    float slow = f32(SLOWMO_A) * f32(SLOWMO_B);
    slow = slow * f32(SLOWMO_C);
    for (uint32_t i = 0; i < 4; i++) {
        float v = dt * all;
        v = v * f32(SCALE_CHANNEL + i * 4);
        if (i != 3) v = slow * v;
        put_f32(DT_CHANNEL + i * 4, v);
    }
}

bool timestep(double fps) {
    if (vm::read32(FIXED_STEP) != 0) return false;
    int32_t mode = static_cast<int32_t>(vm::read32(SMOOTH_MODE));
    if (mode >= 1 && mode <= 4) return false;
    float refresh = f32(REFRESH_HZ);
    if (!refresh_sane(refresh) || static_cast<int32_t>(vm::read32(REFRESH_INT)) <= 0) return false;
    uint64_t now = ppu::timebase();
    uint64_t last = vm::read64(LAST_TB);
    if (!last) last = now;
    uint64_t elapsed = now - last;
    vm::write64(ELAPSED_TB, elapsed);
    vm::write64(LAST_TB, now);
    float floor_ratio = static_cast<float>(static_cast<double>(refresh) / fps);
    uint32_t flags = vm::read32(TIME_FLAGS);
    float ratio;
    if (!(flags & 1) || (flags & 2)) {
        vm::write32(TIME_FLAGS, flags & ~2u);
        ratio = floor_ratio;
    } else {
        float per_vblank = static_cast<float>(static_cast<int64_t>(static_cast<float>(static_cast<double>(tb_freq())) / refresh));
        float centi = static_cast<float>(static_cast<double>(elapsed)) * 100.0f;
        centi = centi / per_vblank;
        centi = std::clamp(centi, 100.0f * floor_ratio, 400.0f);
        ratio = std::max(centi / 100.0f, floor_ratio);
    }
    write_dt(ratio / refresh);
    return true;
}

}

bool fps_unlock_active() { return config().on; }

double fps_unlock_target() {
    const Config& c = config();
    if (!c.on) return 0.0;
    if (c.fps > 0.0) return c.fps;
    float hz = vm::g_base ? f32(REFRESH_HZ) : 0.0f;
    return refresh_sane(hz) ? hz : 59.94;
}

void stats() {
    static const int every = std::getenv("GOWA_FPS_STATS") ? std::atoi(std::getenv("GOWA_FPS_STATS")) : 0;
    if (every <= 0) return;
    static auto start = std::chrono::steady_clock::now();
    static double raw = 0.0, chan = 0.0;
    static uint32_t frames = 0, at_cap = 0, at_floor = 0;
    float hz = f32(REFRESH_HZ);
    float dt = f32(DT_RAW);
    if (refresh_sane(hz) && dt > 0.0f) {
        raw += dt;
        chan += f32(DT_CHANNEL);
        frames++;
        at_cap += dt * hz > 3.99f;
        at_floor += dt * hz < 1.01f;
    }
    auto now = std::chrono::steady_clock::now();
    double wall = std::chrono::duration<double>(now - start).count();
    if (wall < every) return;
    LOG_INFO("fps", "stats %.1f s: %u frames (%.2f fps), game/wall raw %.4f scaled %.4f, dt at 4-vblank cap %u, at 1-vblank floor %u", wall, frames, frames / wall, raw / wall, chan / wall,
             at_cap, at_floor);
    start = now;
    raw = chan = 0.0;
    frames = at_cap = at_floor = 0;
}

bool fps_unlock_timestep(ppu::Context&) {
    stats();
    const Config& c = config();
    if (!c.on || !verified()) return false;
    float refresh = f32(REFRESH_HZ);
    double fps = c.follow_refresh ? (refresh_sane(refresh) ? refresh : 59.94) : c.fps;
    pace(fps);
    if (c.follow_refresh || !refresh_sane(refresh) || fps <= refresh + 0.5) return false;
    return timestep(fps);
}

}
