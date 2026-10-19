#include "spu/thread.h"
#include "runtime/memory.h"
#include "runtime/reservation.h"

#include <intrin.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace {

uint64_t g_sink = 0;

double measure(const char* name, int iters, const std::function<void(int)>& setup, const std::function<void(int)>& op) {
    double best = 1e30;
    for (int trial = 0; trial < 7; trial++) {
        uint64_t total = 0;
        for (int i = 0; i < iters; i++) {
            setup(i);
            uint64_t t0 = __rdtsc();
            op(i);
            total += __rdtsc() - t0;
        }
        best = std::min(best, double(total) / iters);
    }
    uint64_t t0 = __rdtsc();
    for (int i = 0; i < 1000; i++) g_sink += __rdtsc();
    double overhead = double(__rdtsc() - t0) / 1000;
    std::printf("%-44s %8.1f cycles/op (rdtsc pair ~%.0f)\n", name, best, overhead);
    return best;
}

void mfc(spu::Host& h, uint32_t cmd, uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag) {
    h.write_channel(16, lsa);
    h.write_channel(17, 0);
    h.write_channel(18, ea);
    h.write_channel(19, size);
    h.write_channel(20, tag);
    h.write_channel(21, cmd);
}

void tagwait(spu::Host& h, uint32_t mask) {
    h.write_channel(22, mask);
    h.write_channel(23, 2);
    g_sink += h.read_channel(24);
}

}

int main() {
    vm::init();
    const uint32_t base = 0x30000000;
    vm::map(base, 0x01000000);
    for (uint32_t i = 0; i < 0x01000000; i += 4) vm::write32(base + i, i * 2654435761u);
    auto t = std::make_shared<spu::SpuThread>();
    spu::SpuThread& th = *t;
    spu::Host& h = th;
    std::printf("SpuThread host-call costs (TSC cycles, min of 7 trials)\n");
    {
        std::mutex mx;
        static std::vector<uint8_t> a(0x4000), b(0x4000);
        measure("baseline: std::mutex lock+unlock", 200000, [](int) {}, [&](int) { std::lock_guard lk(mx); g_sink++; });
        measure("baseline: memcpy 0x4000", 20000, [](int) {}, [&](int i) { a[0] = uint8_t(i); std::memcpy(b.data(), a.data(), 0x4000); g_sink += b[i & 0x3fff]; });
        measure("baseline: memcpy 0x80", 200000, [](int) {}, [&](int i) { a[0] = uint8_t(i); std::memcpy(b.data(), a.data(), 0x80); g_sink += b[i & 0x7f]; });
        measure("baseline: virtual channel_count(21)", 200000, [](int) {}, [&](int) { g_sink += h.channel_count(21); });
        measure("baseline: rsv::copy_if_changed 0x80 same", 200000, [](int) {}, [&](int) { g_sink += rsv::copy_if_changed(base, th.ls + 0x30000, 0x80); });
        measure("baseline: rsv::store_locked 0x80", 200000, [](int) {}, [&](int i) { rsv::store_locked(base + 0xB00000 + (i & 63) * 0x80, th.ls, 0x80); });
        measure("baseline: mark_ls 0x80", 200000, [](int) {}, [&](int) { th.mark_ls(0x30000, 0x80); });
    }
    measure("wrch MFC param (ch16..20) x5", 200000, [](int) {}, [&](int i) {
        h.write_channel(16, 0x10000);
        h.write_channel(17, 0);
        h.write_channel(18, base + (i & 0xFF) * 0x80);
        h.write_channel(19, 0x80);
        h.write_channel(20, 3);
    });
    measure("GET 0x80 unchanged (params+cmd)", 200000, [](int) {}, [&](int i) { mfc(h, 0x40, 0x10000, base, 0x80, 1); });
    measure("GET 0x80 changed (2 alternating EAs)", 200000, [](int) {}, [&](int i) { mfc(h, 0x40, 0x10000, base + (i & 1) * 0x80, 0x80, 1); });
    measure("GET 0x80 cold EA stride 4K", 20000, [](int) {}, [&](int i) { mfc(h, 0x40, 0x10000, base + 0x100000 + (i % 3000) * 0x1000, 0x80, 1); });
    measure("GET 0x400 changed", 100000, [](int) {}, [&](int i) { mfc(h, 0x40, 0x10000, base + (i & 1) * 0x400, 0x400, 1); });
    measure("GET 0x4000 changed", 20000, [](int) {}, [&](int i) { mfc(h, 0x40, 0x10000, base + (i & 1) * 0x4000, 0x4000, 1); });
    measure("GET 0x4000 unchanged", 20000, [](int) {}, [&](int i) { mfc(h, 0x40, 0x10000, base, 0x4000, 1); });
    measure("PUT 0x80 (no snapshot)", 200000, [&](int) { th.ls[0x20000] ^= 1; }, [&](int i) { mfc(h, 0x20, 0x20000, base + 0x800000 + (i & 63) * 0x80, 0x80, 2); });
    measure("PUT 0x80 after GET (merge path)", 100000, [&](int i) { mfc(h, 0x40, 0x20000, base + 0x810000, 0x80, 2); th.ls[0x20000] ^= 1; }, [&](int i) { mfc(h, 0x20, 0x20000, base + 0x810000, 0x80, 2); });
    measure("PUT 0x4000", 20000, [&](int) { th.ls[0x24000] ^= 1; }, [&](int i) { mfc(h, 0x20, 0x24000, base + 0x900000 + (i & 7) * 0x4000, 0x4000, 2); });
    {
        uint32_t list = 0x28f50;
        uint32_t sizes[22] = {0x250, 0x940, 0x610, 0x3c0, 0x10, 0x250, 0, 0, 0, 0, 0, 0x25b0, 0, 0x10, 0x20, 0, 0x50, 0x110, 0x40, 0, 0, 0};
        for (int k = 0; k < 22; k++) {
            uint32_t e[2] = {__builtin_bswap32(sizes[k]), __builtin_bswap32(base + 0x200000 + k * 0x4000)};
            std::memcpy(th.ls + list + k * 8, e, 8);
        }
        measure("GETL 22 elems ~0x2d00 B (Edge gather), new data", 20000, [&](int i) { vm::write32(base + 0x200000 + (i % 22) * 0x4000, i); for (int k = 0; k < 22; k += 3) vm::write32(base + 0x200000 + k * 0x4000 + 0x10, i); }, [&](int i) { mfc(h, 0x44, 0x1d000, list, 0xb0, 1); });
    }
    measure("tag wait (wrch22, wrch23, rdch24)", 200000, [](int) {}, [&](int) { tagwait(h, 2); });
    measure("rdch ch8 decrementer", 200000, [](int) {}, [&](int) { g_sink += h.read_channel(8); });
    measure("rchcnt ch23", 200000, [](int) {}, [&](int) { g_sink += h.channel_count(23); });
    measure("rdch ch13 (machine status)", 200000, [](int) {}, [&](int) { g_sink += h.read_channel(13); });
    measure("GETLLAR + rdch27", 200000, [](int) {}, [&](int i) {
        mfc(h, 0xD0, 0x80, base + 0xA00000, 0x80, 0);
        g_sink += h.read_channel(27);
    });
    measure("GETLLAR + PUTLLC(ok) + 2x rdch27", 100000, [](int) {}, [&](int i) {
        mfc(h, 0xD0, 0x80, base + 0xA00000, 0x80, 0);
        g_sink += h.read_channel(27);
        th.ls[0x80 + 0x70] ^= 1;
        mfc(h, 0xB4, 0x80, base + 0xA00000, 0x80, 0);
        g_sink += h.read_channel(27);
    });
    measure("PUTLLUC + rdch27", 200000, [&](int i) { th.ls[0x100] ^= 1; }, [&](int i) {
        mfc(h, 0xB0, 0x100, base + 0xA00080, 0x80, 0);
        g_sink += h.read_channel(27);
    });
    measure("wrch ch2 + rdch ch0 (event ack/read, no event)", 100000, [](int) {}, [&](int) {
        h.write_channel(2, 0);
        g_sink += h.channel_count(0);
    });
    std::printf("sink %llu\n", (unsigned long long)(g_sink & 1));
    std::fflush(stdout);
    std::_Exit(0);
}
