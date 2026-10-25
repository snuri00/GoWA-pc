#include "core.h"

#include "spu/hle.h"

#include "runtime/log.h"
#include "runtime/memory.h"

#include <atomic>
#include <cstring>
#include <string>

namespace jobsys {

namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

std::atomic<int> g_logged{0};
constexpr int kMaxLog = 400;

bool on_getbuffer(spu::Context& c) {
    if (g_logged.load(std::memory_order_relaxed) >= kMaxLog) return false;
    if (be32(c.ls + 0x4000 + 0x10) != 0x7f60) return false;
    g_logged.fetch_add(1, std::memory_order_relaxed);
    std::string cmd;
    for (uint32_t off = 0; off < 0x60; off += 4) {
        char b[12];
        std::snprintf(b, sizeof b, " %08x", be32(c.ls + 0xc00 + off));
        cmd += b;
    }
    LOG_INFO("jstrace", "GetBuffer r3 %08x r4 %08x r5 %08x r6 %08x lr %05x cmd%s", c.gpr[3].u32[3], c.gpr[4].u32[3], c.gpr[5].u32[3], c.gpr[6].u32[3], c.gpr[0].u32[3], cmd.c_str());
    return false;
}

bool on_ret(spu::Context& c) {
    if (g_logged.load(std::memory_order_relaxed) >= kMaxLog) return false;
    g_logged.fetch_add(1, std::memory_order_relaxed);
    LOG_INFO("jstrace", "GetBuffer returned r3 %08x at %05x", c.gpr[3].u32[3], c.pc);
    return false;
}

uint32_t g_sig_gb[8], g_sig_r1[6], g_sig_r2[6];

}

void install_trace() {
    const char* e = std::getenv("GOWA_JOBSYS_TRACE_GETBUF");
    if (!e || e[0] == '0') return;
    for (int i = 0; i < 8; i++) g_sig_gb[i] = vm::read32(kPmEa + 0x2fd8 - 0xa00 + 4 * i);
    for (int i = 0; i < 6; i++) g_sig_r1[i] = vm::read32(0x728400 + 0xbde8 - 0x4000 + 4 * i);
    for (int i = 0; i < 6; i++) g_sig_r2[i] = vm::read32(0x728400 + 0xbe28 - 0x4000 + 4 * i);
    spu::hle::register_function({0x2fd8, 8, g_sig_gb, on_getbuffer, "trace GetBuffer"});
    spu::hle::register_function({0xbde8, 6, g_sig_r1, on_ret, "trace GetBuffer ret 1"});
    spu::hle::register_function({0xbe28, 6, g_sig_r2, on_ret, "trace GetBuffer ret 2"});
}

}
