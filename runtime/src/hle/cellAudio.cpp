#include "runtime/placement.h"
#include "runtime/host.h"
#include "ppu/ops.h"
#include "runtime/hostsleep.h"
#include "common.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#pragma comment(lib, "ole32.lib")
#else
#include <SDL3/SDL.h>
#endif
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

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

#ifdef _WIN32
struct Output {
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    UINT32 frames = 0;
    bool ok = false;

    bool open() {
        if (const char* e = std::getenv("GOWA_AUDIO"); e && e[0] == '0') return false;
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return false;
        IMMDeviceEnumerator* en = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en)))) return false;
        IMMDevice* dev = nullptr;
        HRESULT hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        en->Release();
        if (FAILED(hr)) return false;
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
        dev->Release();
        if (FAILED(hr)) return false;
        WAVEFORMATEXTENSIBLE wf{};
        wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wf.Format.nChannels = 2;
        wf.Format.nSamplesPerSec = 48000;
        wf.Format.wBitsPerSample = 32;
        wf.Format.nBlockAlign = 8;
        wf.Format.nAvgBytesPerSec = 48000 * 8;
        wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wf.Samples.wValidBitsPerSample = 32;
        wf.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        wf.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 1000000, 0,
                                reinterpret_cast<WAVEFORMATEX*>(&wf), nullptr);
        if (FAILED(hr)) {
            LOG_WARN("cellAudio", "WASAPI initialize failed 0x%08lx", static_cast<unsigned long>(hr));
            return false;
        }
        client->GetBufferSize(&frames);
        if (FAILED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render)))) return false;
        client->Start();
        ok = true;
        LOG_INFO("cellAudio", "WASAPI output opened, buffer %u frames", frames);
        return true;
    }

    void write(const float* stereo, UINT32 n) {
        if (!ok) return;
        UINT32 pad = 0;
        if (FAILED(client->GetCurrentPadding(&pad))) return;
        UINT32 room = frames > pad ? frames - pad : 0;
        static const UINT32 target = std::getenv("GOWA_AUDIO_LATENCY_MS") ? static_cast<UINT32>(std::atoi(std::getenv("GOWA_AUDIO_LATENCY_MS")) * 48) : 4800;
        if (pad > target) return;
        if (n > room) n = room;
        if (!n) return;
        BYTE* dst = nullptr;
        if (FAILED(render->GetBuffer(n, &dst))) return;
        std::memcpy(dst, stereo, size_t(n) * 8);
        render->ReleaseBuffer(n, 0);
    }
};
#else
struct Output {
    SDL_AudioStream* stream = nullptr;

    bool open() {
        if (const char* e = std::getenv("GOWA_AUDIO"); e && e[0] == '0') return false;
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            LOG_WARN("cellAudio", "SDL audio init failed: %s", SDL_GetError());
            return false;
        }
        SDL_AudioSpec spec{SDL_AUDIO_F32, 2, 48000};
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!stream) {
            LOG_WARN("cellAudio", "SDL audio open failed: %s", SDL_GetError());
            return false;
        }
        SDL_ResumeAudioStreamDevice(stream);
        LOG_INFO("cellAudio", "SDL audio output opened (%s)", SDL_GetCurrentAudioDriver());
        return true;
    }

    void write(const float* stereo, uint32_t n) {
        if (!stream) return;
        static const int target = std::getenv("GOWA_AUDIO_LATENCY_MS") ? std::atoi(std::getenv("GOWA_AUDIO_LATENCY_MS")) * 48 : 4800;
        if (SDL_GetAudioStreamQueued(stream) / 8 > target) return;
        SDL_PutAudioStreamData(stream, stereo, static_cast<int>(n * 8));
    }
};
#endif

float be_float(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    v = __builtin_bswap32(v);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void mix_block(const Port& p, uint64_t block, float* stereo) {
    const uint8_t* src = vm::ptr(p.addr) + size_t(block) * p.channels * 256 * 4;
    const float c = 0.7071f;
    for (uint32_t s = 0; s < 256; s++) {
        const uint8_t* f = src + size_t(s) * p.channels * 4;
        float l = 0.0f, r = 0.0f;
        if (p.channels == 1) {
            l = r = be_float(f);
        } else if (p.channels == 2) {
            l = be_float(f);
            r = be_float(f + 4);
        } else {
            float fl = be_float(f), fr = be_float(f + 4), fc = be_float(f + 8);
            float bl = be_float(f + 16), br = be_float(f + 20);
            float sl = p.channels >= 8 ? be_float(f + 24) : 0.0f, sr = p.channels >= 8 ? be_float(f + 28) : 0.0f;
            l = fl + c * fc + c * bl + c * sl;
            r = fr + c * fc + c * br + c * sr;
        }
        stereo[s * 2] += l;
        stereo[s * 2 + 1] += r;
    }
}

void audio_thread() {
    host::set_thread_name("cellAudio");
    placement::apply("audio");
    Output out;
    out.open();
    auto next = std::chrono::steady_clock::now();
    const auto period = std::chrono::microseconds(5333);
    std::vector<float> stereo(512);
    while (g_init) {
        next += std::chrono::microseconds(static_cast<int64_t>(period.count() / ppu::time_scale()));
        host::sleep_until_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(next.time_since_epoch()).count());
        std::fill(stereo.begin(), stereo.end(), 0.0f);
        bool any = false;
        {
            std::lock_guard lk(g_lock);
            for (uint32_t i = 0; i < 8; i++) {
                Port& p = g_ports[i];
                if (!p.open || !p.running || !p.blocks) continue;
                if (p.addr && vm::range_mapped(p.addr, p.size)) {
                    mix_block(p, p.index, stereo.data());
                    any = true;
                }
                p.index = (p.index + 1) % p.blocks;
                vm::write64(g_index_area + i * 8, p.index);
            }
        }
        if (any && ppu::time_scale() == 1.0) {
            for (float& v : stereo) v = std::clamp(v * 0.7f, -1.0f, 1.0f);
            out.write(stereo.data(), 256);
            static FILE* dump = std::getenv("GOWA_AUDIO_DUMP") ? std::fopen(std::getenv("GOWA_AUDIO_DUMP"), "wb") : nullptr;
            if (dump) std::fwrite(stereo.data(), 4, stereo.size(), dump);
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
