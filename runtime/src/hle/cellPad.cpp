#include "common.h"

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace host {
void poll_pad(uint16_t* b);
}

namespace hle {

namespace {

struct Press {
    double at;
    double hold;
    uint32_t offset;
    uint16_t mask;
    uint32_t pressure;
    uint16_t axis_value;
};

bool g_init = false;
uint32_t g_max = 7;
uint32_t g_setting = 0;
std::once_flag g_script_once;
std::vector<Press> g_script;
std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();

bool axis_of(const std::string& name, uint32_t& offset, uint16_t& value) {
    if (name.size() != 3 || (name[2] != '+' && name[2] != '-')) return false;
    std::string a = name.substr(0, 2);
    if (a == "rx") offset = 4;
    else if (a == "ry") offset = 5;
    else if (a == "lx") offset = 6;
    else if (a == "ly") offset = 7;
    else return false;
    value = name[2] == '+' ? 0xFF : 0x00;
    return true;
}

bool button_of(const std::string& name, uint32_t& offset, uint16_t& mask, uint32_t& pressure) {
    struct B { const char* n; uint32_t o; uint16_t m; uint32_t p; };
    static const B table[] = {
        {"select", 2, 0x01, 0}, {"l3", 2, 0x02, 0}, {"r3", 2, 0x04, 0}, {"start", 2, 0x08, 0},
        {"up", 2, 0x10, 10}, {"right", 2, 0x20, 8}, {"down", 2, 0x40, 11}, {"left", 2, 0x80, 9},
        {"l2", 3, 0x01, 18}, {"r2", 3, 0x02, 19}, {"l1", 3, 0x04, 16}, {"r1", 3, 0x08, 17},
        {"triangle", 3, 0x10, 12}, {"circle", 3, 0x20, 13}, {"cross", 3, 0x40, 14}, {"square", 3, 0x80, 15},
    };
    for (const B& b : table) {
        if (name == b.n) {
            offset = b.o;
            mask = b.m;
            pressure = b.p;
            return true;
        }
    }
    return false;
}

void load_script() {
    const char* env = std::getenv("GOWA_AUTOPAD");
    if (!env) return;
    std::string s = env;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        std::string item = s.substr(pos, end - pos);
        size_t at = item.find('@');
        Press p{};
        bool axis = at != std::string::npos && axis_of(item.substr(0, at), p.offset, p.axis_value);
        if (axis) p.mask = 0;
        if (at != std::string::npos && (axis || button_of(item.substr(0, at), p.offset, p.mask, p.pressure))) {
            std::string when = item.substr(at + 1);
            size_t colon = when.find(':');
            p.at = std::atof(when.substr(0, colon).c_str());
            p.hold = colon == std::string::npos ? 0.25 : std::atof(when.substr(colon + 1).c_str());
            g_script.push_back(p);
            LOG_INFO("cellPad", "autopad: %s at %.1fs", item.substr(0, at).c_str(), p.at);
        }
        pos = end + 1;
    }
}

void fill(uint32_t out) {
    std::call_once(g_script_once, load_script);
    double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    uint16_t b[24] = {};
    b[1] = 0x70 | 12;
    b[4] = b[5] = b[6] = b[7] = 0x80;
    b[20] = b[21] = b[22] = 0x200;
    b[23] = 0x200;
    for (const Press& p : g_script) {
        if (now >= p.at && now < p.at + p.hold) {
            if (!p.mask) {
                b[p.offset] = p.axis_value;
                continue;
            }
            b[p.offset] |= p.mask;
            if (p.pressure) b[p.pressure] = 0xFF;
        }
    }
    host::poll_pad(b);
    for (const Press& p : g_script) {
        if (!p.mask && now >= p.at && now < p.at + p.hold) b[p.offset] = p.axis_value;
    }
    vm::write32(out, 24);
    for (int i = 0; i < 24; i++) vm::write16(out + 4 + i * 2, b[i]);
}

}

HLE_FUNC(sys_io, cellPadInit) {
    if (g_init) return ret(ctx, static_cast<int32_t>(0x80121103));
    g_init = true;
    g_max = a32(ctx, 0) > 7 ? 7 : a32(ctx, 0);
    g_start = std::chrono::steady_clock::now();
    ret(ctx, 0);
}

HLE_FUNC(sys_io, cellPadEnd) {
    g_init = false;
    ret(ctx, 0);
}

HLE_FUNC(sys_io, cellPadGetInfo2) {
    uint32_t out = a32(ctx, 0);
    if (!g_init) return ret(ctx, static_cast<int32_t>(0x80121104));
    std::memset(vm::ptr(out), 0, 4 * 3 + 4 * 7 * 4);
    vm::write32(out, g_max);
    vm::write32(out + 4, 1);
    vm::write32(out + 8, 0);
    vm::write32(out + 12, 1);
    vm::write32(out + 12 + 28, g_setting);
    vm::write32(out + 12 + 56, 0x7);
    vm::write32(out + 12 + 84, 0);
    ret(ctx, 0);
}

HLE_FUNC(sys_io, cellPadGetData) {
    uint32_t port = a32(ctx, 0), out = a32(ctx, 1);
    if (!g_init) return ret(ctx, static_cast<int32_t>(0x80121104));
    if (port >= g_max) return ret(ctx, static_cast<int32_t>(0x80121107));
    std::memset(vm::ptr(out), 0, 4 + 64 * 2);
    if (port != 0) return ret(ctx, static_cast<int32_t>(0x80121107));
    fill(out);
    ret(ctx, 0);
}

HLE_FUNC(sys_io, cellPadClearBuf) { ret(ctx, 0); }

HLE_FUNC(sys_io, cellPadSetPortSetting) {
    if (a32(ctx, 0) == 0) g_setting = a32(ctx, 1);
    ret(ctx, 0);
}

HLE_FUNC(sys_io, cellPadSetActDirect) { ret(ctx, 0); }

}
