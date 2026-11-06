#include "runtime/log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>
#else
#include <SDL3/SDL.h>
#endif

namespace host {

namespace {

std::atomic<uint32_t> g_rumble{0};
uint32_t g_rumble_sent = 0;

bool rumble_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("GOWA_VIBRATION");
        return !e || !e[0] || e[0] != '0';
    }();
    return on;
}

void press(uint16_t* b, uint32_t offset, uint16_t mask, uint32_t pressure) {
    b[offset] |= mask;
    if (pressure) b[pressure] = 0xFF;
}

void stick(int sx, int sy, int deadzone, uint16_t& ox, uint16_t& oy) {
    float x = static_cast<float>(sx), y = static_cast<float>(sy);
    float mag = std::sqrt(x * x + y * y);
    if (mag <= deadzone) {
        ox = oy = 128;
        return;
    }
    float scale = (std::min(mag, 32767.0f) - deadzone) / (32767.0f - deadzone) / mag;
    x *= scale;
    y *= scale;
    ox = static_cast<uint16_t>(std::clamp(static_cast<int>(std::lround(128.0f + x * 127.0f)), 0, 255));
    oy = static_cast<uint16_t>(std::clamp(static_cast<int>(std::lround(128.0f - y * 127.0f)), 0, 255));
}

#ifdef _WIN32

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputSetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);

XInputGetStateFn g_xinput = nullptr;
XInputSetStateFn g_xinput_set = nullptr;
std::once_flag g_once;

void load() {
    const wchar_t* names[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
    for (const wchar_t* n : names) {
        if (HMODULE m = LoadLibraryW(n)) {
            g_xinput = reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState"));
            g_xinput_set = reinterpret_cast<XInputSetStateFn>(GetProcAddress(m, "XInputSetState"));
            if (g_xinput) return;
        }
    }
}

bool focused() {
    HWND w = GetForegroundWindow();
    if (!w) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

bool key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

int g_slot = -1;
ULONGLONG g_next_scan = 0;
#endif

}

void set_rumble(uint8_t small, uint8_t large) {
    if (!rumble_enabled()) return;
    g_rumble.store(static_cast<uint32_t>(large) << 8 | (small ? 0xFFu : 0u), std::memory_order_relaxed);
}

#ifdef _WIN32

void poll_pad(uint16_t* b) {
    std::call_once(g_once, load);
    if (g_xinput) {
        XINPUT_STATE st{};
        bool have = g_slot >= 0 && g_xinput(static_cast<DWORD>(g_slot), &st) == ERROR_SUCCESS;
        if (!have && GetTickCount64() >= g_next_scan) {
            g_slot = -1;
            for (DWORD i = 0; i < 4 && !have; i++) {
                if (g_xinput(i, &st) == ERROR_SUCCESS) {
                    g_slot = static_cast<int>(i);
                    have = true;
                }
            }
            if (!have) g_next_scan = GetTickCount64() + 1000;
        }
        if (have) {
            WORD w = st.Gamepad.wButtons;
            if (w & XINPUT_GAMEPAD_BACK) press(b, 2, 0x01, 0);
            if (w & XINPUT_GAMEPAD_LEFT_THUMB) press(b, 2, 0x02, 0);
            if (w & XINPUT_GAMEPAD_RIGHT_THUMB) press(b, 2, 0x04, 0);
            if (w & XINPUT_GAMEPAD_START) press(b, 2, 0x08, 0);
            if (w & XINPUT_GAMEPAD_DPAD_UP) press(b, 2, 0x10, 10);
            if (w & XINPUT_GAMEPAD_DPAD_RIGHT) press(b, 2, 0x20, 8);
            if (w & XINPUT_GAMEPAD_DPAD_DOWN) press(b, 2, 0x40, 11);
            if (w & XINPUT_GAMEPAD_DPAD_LEFT) press(b, 2, 0x80, 9);
            if (st.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) {
                b[3] |= 0x01;
                b[18] = st.Gamepad.bLeftTrigger;
            }
            if (st.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) {
                b[3] |= 0x02;
                b[19] = st.Gamepad.bRightTrigger;
            }
            if (w & XINPUT_GAMEPAD_LEFT_SHOULDER) press(b, 3, 0x04, 16);
            if (w & XINPUT_GAMEPAD_RIGHT_SHOULDER) press(b, 3, 0x08, 17);
            if (w & XINPUT_GAMEPAD_Y) press(b, 3, 0x10, 12);
            if (w & XINPUT_GAMEPAD_B) press(b, 3, 0x20, 13);
            if (w & XINPUT_GAMEPAD_A) press(b, 3, 0x40, 14);
            if (w & XINPUT_GAMEPAD_X) press(b, 3, 0x80, 15);
            stick(st.Gamepad.sThumbRX, st.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, b[4], b[5]);
            stick(st.Gamepad.sThumbLX, st.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, b[6], b[7]);
            uint32_t r = g_rumble.load(std::memory_order_relaxed);
            if (g_xinput_set && r != g_rumble_sent) {
                XINPUT_VIBRATION v{};
                v.wLeftMotorSpeed = static_cast<WORD>((r >> 8 & 0xFF) * 257);
                v.wRightMotorSpeed = static_cast<WORD>((r & 0xFF) * 257);
                if (g_xinput_set(static_cast<DWORD>(g_slot), &v) == ERROR_SUCCESS) g_rumble_sent = r;
            }
        }
    }
    if (!focused()) return;
    if (key(VK_BACK)) press(b, 2, 0x01, 0);
    if (key(VK_RETURN)) press(b, 2, 0x08, 0);
    if (key(VK_UP)) press(b, 2, 0x10, 10);
    if (key(VK_RIGHT)) press(b, 2, 0x20, 8);
    if (key(VK_DOWN)) press(b, 2, 0x40, 11);
    if (key(VK_LEFT)) press(b, 2, 0x80, 9);
    if (key('1')) press(b, 3, 0x01, 18);
    if (key('3')) press(b, 3, 0x02, 19);
    if (key('Q')) press(b, 3, 0x04, 16);
    if (key('E')) press(b, 3, 0x08, 17);
    if (key('S')) press(b, 3, 0x10, 12);
    if (key('X')) press(b, 3, 0x20, 13);
    if (key('Z') || key(VK_SPACE)) press(b, 3, 0x40, 14);
    if (key('A')) press(b, 3, 0x80, 15);
    if (key('J')) b[6] = 0;
    if (key('L')) b[6] = 255;
    if (key('I')) b[7] = 0;
    if (key('K')) b[7] = 255;
}

#else

namespace {

const bool* keys() {
    int n = 0;
    return SDL_GetKeyboardState(&n);
}

bool key(SDL_Scancode sc) {
    const bool* k = keys();
    return k && k[sc];
}

SDL_Gamepad* pad() {
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    SDL_Gamepad* p = nullptr;
    for (int i = 0; i < n && !p; i++) p = SDL_GetGamepadFromID(ids[i]);
    SDL_free(ids);
    return p;
}

}

}

namespace gpu {
void* native_window();
}

namespace host {

void poll_pad(uint16_t* b) {
    void* w = gpu::native_window();
    if (!w) return;
    if (SDL_Gamepad* p = pad()) {
        auto btn = [&](SDL_GamepadButton x) { return SDL_GetGamepadButton(p, x); };
        if (btn(SDL_GAMEPAD_BUTTON_BACK)) press(b, 2, 0x01, 0);
        if (btn(SDL_GAMEPAD_BUTTON_LEFT_STICK)) press(b, 2, 0x02, 0);
        if (btn(SDL_GAMEPAD_BUTTON_RIGHT_STICK)) press(b, 2, 0x04, 0);
        if (btn(SDL_GAMEPAD_BUTTON_START)) press(b, 2, 0x08, 0);
        if (btn(SDL_GAMEPAD_BUTTON_DPAD_UP)) press(b, 2, 0x10, 10);
        if (btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) press(b, 2, 0x20, 8);
        if (btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) press(b, 2, 0x40, 11);
        if (btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) press(b, 2, 0x80, 9);
        int lt = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7, rt = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7;
        if (lt > 30) {
            b[3] |= 0x01;
            b[18] = static_cast<uint16_t>(std::min(lt, 255));
        }
        if (rt > 30) {
            b[3] |= 0x02;
            b[19] = static_cast<uint16_t>(std::min(rt, 255));
        }
        if (btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) press(b, 3, 0x04, 16);
        if (btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) press(b, 3, 0x08, 17);
        if (btn(SDL_GAMEPAD_BUTTON_NORTH)) press(b, 3, 0x10, 12);
        if (btn(SDL_GAMEPAD_BUTTON_EAST)) press(b, 3, 0x20, 13);
        if (btn(SDL_GAMEPAD_BUTTON_SOUTH)) press(b, 3, 0x40, 14);
        if (btn(SDL_GAMEPAD_BUTTON_WEST)) press(b, 3, 0x80, 15);
        auto axis = [&](SDL_GamepadAxis a) { return std::clamp(static_cast<int>(SDL_GetGamepadAxis(p, a)), -32767, 32767); };
        stick(axis(SDL_GAMEPAD_AXIS_RIGHTX), -axis(SDL_GAMEPAD_AXIS_RIGHTY), 8689, b[4], b[5]);
        stick(axis(SDL_GAMEPAD_AXIS_LEFTX), -axis(SDL_GAMEPAD_AXIS_LEFTY), 7849, b[6], b[7]);
        uint32_t r = g_rumble.load(std::memory_order_relaxed);
        if (r || g_rumble_sent) {
            static bool warned = false;
            if (!SDL_RumbleGamepad(p, static_cast<Uint16>((r >> 8 & 0xFF) * 257), static_cast<Uint16>((r & 0xFF) * 257), r ? 250 : 0) && !warned) {
                warned = true;
                LOG_WARN("input", "gamepad rumble failed: %s", SDL_GetError());
            }
            g_rumble_sent = r;
        }
    }
    if (SDL_GetKeyboardFocus() != static_cast<SDL_Window*>(w)) return;
    if (key(SDL_SCANCODE_BACKSPACE)) press(b, 2, 0x01, 0);
    if (key(SDL_SCANCODE_RETURN)) press(b, 2, 0x08, 0);
    if (key(SDL_SCANCODE_UP)) press(b, 2, 0x10, 10);
    if (key(SDL_SCANCODE_RIGHT)) press(b, 2, 0x20, 8);
    if (key(SDL_SCANCODE_DOWN)) press(b, 2, 0x40, 11);
    if (key(SDL_SCANCODE_LEFT)) press(b, 2, 0x80, 9);
    if (key(SDL_SCANCODE_1)) press(b, 3, 0x01, 18);
    if (key(SDL_SCANCODE_3)) press(b, 3, 0x02, 19);
    if (key(SDL_SCANCODE_Q)) press(b, 3, 0x04, 16);
    if (key(SDL_SCANCODE_E)) press(b, 3, 0x08, 17);
    if (key(SDL_SCANCODE_S)) press(b, 3, 0x10, 12);
    if (key(SDL_SCANCODE_X)) press(b, 3, 0x20, 13);
    if (key(SDL_SCANCODE_Z) || key(SDL_SCANCODE_SPACE)) press(b, 3, 0x40, 14);
    if (key(SDL_SCANCODE_A)) press(b, 3, 0x80, 15);
    if (key(SDL_SCANCODE_J)) b[6] = 0;
    if (key(SDL_SCANCODE_L)) b[6] = 255;
    if (key(SDL_SCANCODE_I)) b[7] = 0;
    if (key(SDL_SCANCODE_K)) b[7] = 255;
}

#endif

}
