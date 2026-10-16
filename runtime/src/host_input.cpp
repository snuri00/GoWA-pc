#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <cstdint>
#include <mutex>

namespace host {

namespace {

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

XInputGetStateFn g_xinput = nullptr;
std::once_flag g_once;

void load() {
    const wchar_t* names[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
    for (const wchar_t* n : names) {
        if (HMODULE m = LoadLibraryW(n)) {
            g_xinput = reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState"));
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

uint16_t axis(SHORT v, bool invert) {
    int x = v / 256 + 128;
    if (invert) x = 255 - x;
    return static_cast<uint16_t>(std::clamp(x, 0, 255));
}

void press(uint16_t* b, uint32_t offset, uint16_t mask, uint32_t pressure) {
    b[offset] |= mask;
    if (pressure) b[pressure] = 0xFF;
}

}

void poll_pad(uint16_t* b) {
    std::call_once(g_once, load);
    if (g_xinput) {
        XINPUT_STATE st{};
        if (g_xinput(0, &st) == ERROR_SUCCESS) {
            WORD w = st.Gamepad.wButtons;
            if (w & XINPUT_GAMEPAD_BACK) press(b, 2, 0x01, 0);
            if (w & XINPUT_GAMEPAD_LEFT_THUMB) press(b, 2, 0x02, 0);
            if (w & XINPUT_GAMEPAD_RIGHT_THUMB) press(b, 2, 0x04, 0);
            if (w & XINPUT_GAMEPAD_START) press(b, 2, 0x08, 0);
            if (w & XINPUT_GAMEPAD_DPAD_UP) press(b, 2, 0x10, 10);
            if (w & XINPUT_GAMEPAD_DPAD_RIGHT) press(b, 2, 0x20, 8);
            if (w & XINPUT_GAMEPAD_DPAD_DOWN) press(b, 2, 0x40, 11);
            if (w & XINPUT_GAMEPAD_DPAD_LEFT) press(b, 2, 0x80, 9);
            if (st.Gamepad.bLeftTrigger > 64) press(b, 3, 0x01, 18);
            if (st.Gamepad.bRightTrigger > 64) press(b, 3, 0x02, 19);
            if (w & XINPUT_GAMEPAD_LEFT_SHOULDER) press(b, 3, 0x04, 16);
            if (w & XINPUT_GAMEPAD_RIGHT_SHOULDER) press(b, 3, 0x08, 17);
            if (w & XINPUT_GAMEPAD_Y) press(b, 3, 0x10, 12);
            if (w & XINPUT_GAMEPAD_B) press(b, 3, 0x20, 13);
            if (w & XINPUT_GAMEPAD_A) press(b, 3, 0x40, 14);
            if (w & XINPUT_GAMEPAD_X) press(b, 3, 0x80, 15);
            b[4] = axis(st.Gamepad.sThumbRX, false);
            b[5] = axis(st.Gamepad.sThumbRY, true);
            b[6] = axis(st.Gamepad.sThumbLX, false);
            b[7] = axis(st.Gamepad.sThumbLY, true);
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

}
