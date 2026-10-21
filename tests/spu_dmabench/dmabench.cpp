#include "spu/thread.h"
#include "runtime/memory.h"
#include "runtime/reservation.h"
#include "ppu/ops.h"
#include "ppu/context.h"

#include <intrin.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
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

uint32_t wrap_addr(uint32_t base) {
    for (uint32_t l = base >> 7;; l++)
        if (((l / 7) & 0x3FFF) == 0x3FFF && l % 7 == 6) return (l + 1) << 7;
}

int stress(uint32_t base, int iters, bool tight, bool wrap) {
    const uint32_t shared = wrap ? wrap_addr(base) - 0x400 : base + 0xC10000;
    const uint32_t counter = tight ? shared - 0x80 : base + 0xC00000;
    const uint32_t ppu_word = tight ? shared + 0x800 : base + 0xC20000;
    vm::write32(counter, 0);
    vm::write32(ppu_word, 0);
    for (uint32_t i = 0; i < 0x800; i++) vm::ptr(shared)[i] = 0;
    std::atomic<uint64_t> torn{0}, failed{0}, retries{0};
    std::vector<std::thread> ts;
    const int atomic_threads = 4, copy_threads = 4;
    for (int k = 0; k < atomic_threads; k++)
        ts.emplace_back([&, k] {
            auto t = std::make_shared<spu::SpuThread>();
            spu::Host& h = *t;
            for (int i = 0; i < iters; i++) {
                for (;;) {
                    mfc(h, 0xD0, 0x80, counter, 0x80, 0);
                    h.read_channel(27);
                    uint32_t v;
                    std::memcpy(&v, t->ls + 0x80, 4);
                    v = __builtin_bswap32(__builtin_bswap32(v) + 1);
                    std::memcpy(t->ls + 0x80, &v, 4);
                    mfc(h, 0xB4, 0x80, counter, 0x80, 0);
                    if (h.read_channel(27) == 0) break;
                    retries++;
                }
                ppu::Context pc{};
                for (;;) {
                    uint32_t v = ppu::lwarx(pc, vm::g_base, ppu_word);
                    if (ppu::stwcx(pc, vm::g_base, ppu_word, v + 1)) break;
                    retries++;
                }
            }
        });
    for (int k = 0; k < copy_threads; k++)
        ts.emplace_back([&, k] {
            auto t = std::make_shared<spu::SpuThread>();
            spu::Host& h = *t;
            for (int i = 0; i < iters; i++) {
                uint8_t fill = uint8_t(k * 64 + (i & 63));
                std::memset(t->ls + 0x10000, fill, 0x800);
                uint32_t off = (i * 0x80) & 0x780;
                if (i & 1) mfc(h, 0x20, 0x10000, shared, 0x800, 1);
                else if (i & 2) mfc(h, 0x20, 0x10000 + off, shared + off, 0x80, 1);
                else {
                    mfc(h, 0xD0, 0x20000, shared + off, 0x80, 0);
                    h.read_channel(27);
                    std::memset(t->ls + 0x20000, fill, 0x80);
                    mfc(h, 0xB0, 0x20000, shared + off, 0x80, 0);
                    h.read_channel(27);
                }
                mfc(h, 0x40, 0x18000, shared, 0x800, 2);
                for (uint32_t l = 0; l < 0x800; l += 0x80) {
                    const uint8_t* p = t->ls + 0x18000 + l;
                    for (int b = 1; b < 128; b++)
                        if (p[b] != p[0]) {
                            torn++;
                            break;
                        }
                }
            }
        });
    for (auto& t : ts) t.join();
    uint32_t got = vm::read32(counter), want = uint32_t(atomic_threads * iters);
    uint32_t pgot = vm::read32(ppu_word);
    std::printf("stress: putllc counter %u/%u, ppu stwcx counter %u/%u, retries %llu, torn lines %llu\n", got, want, pgot, want, (unsigned long long)retries.load(), (unsigned long long)torn.load());
    return got == want && pgot == want && torn == 0 ? 0 : 1;
}

int park_ring(uint32_t base, int rounds, int n, int hold_us) {
    const uint32_t line = base + 0xC30000;
    for (uint32_t i = 0; i < 128; i += 4) vm::write32(line + i, 0);
    const uint32_t total = uint32_t(rounds * n);
    std::atomic<uint64_t> waits{0}, slow{0}, max_us{0}, retries{0};
    std::atomic<bool> done{false};
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> ts;
    std::vector<std::shared_ptr<spu::SpuThread>> threads;
    for (int k = 0; k < n; k++) threads.push_back(std::make_shared<spu::SpuThread>());
    for (int k = 0; k < n; k++)
        ts.emplace_back([&, k] {
            auto t = threads[k];
            spu::Host& h = *t;
            ppu::Context pc{};
            h.write_channel(1, 0x400);
            for (;;) {
                mfc(h, 0xD0, 0x80, line, 0x80, 0);
                h.read_channel(27);
                uint32_t v;
                std::memcpy(&v, t->ls + 0x80, 4);
                v = __builtin_bswap32(v);
                if (v >= total) break;
                if (v % uint32_t(n) == uint32_t(k)) {
                    if (hold_us && (v % 3) == 0) {
                        auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(hold_us * int((v / 3) % 4));
                        while (std::chrono::steady_clock::now() < until) _mm_pause();
                    }
                    int how = int((v / uint32_t(n)) % 4);
                    bool ok = false;
                    if (how == 0) {
                        uint32_t nv = __builtin_bswap32(v + 1);
                        std::memcpy(t->ls + 0x80, &nv, 4);
                        mfc(h, 0xB4, 0x80, line, 0x80, 0);
                        ok = h.read_channel(27) == 0;
                    } else if (how == 1) {
                        uint32_t cur = ppu::lwarx(pc, vm::g_base, line);
                        ok = cur == v && ppu::stwcx(pc, vm::g_base, line, v + 1);
                    } else if (how == 2) {
                        vm::write32(line, v + 1);
                        ok = true;
                    } else {
                        uint32_t nv = __builtin_bswap32(v + 1);
                        std::memcpy(t->ls + 0x80, &nv, 4);
                        mfc(h, 0xB0, 0x80, line, 0x80, 0);
                        h.read_channel(27);
                        ok = true;
                    }
                    if (!ok) retries++;
                    continue;
                }
                auto w0 = std::chrono::steady_clock::now();
                uint32_t e = h.read_channel(0);
                h.write_channel(2, e);
                uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - w0).count();
                waits++;
                if (us >= uint64_t(hold_us) * 3 + 1500) slow++;
                uint64_t m = max_us.load();
                while (us > m && !max_us.compare_exchange_weak(m, us)) {
                }
            }
            h.write_channel(1, 0);
        });
    std::thread mbox([&] {
        uint32_t i = 0;
        while (!done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            threads[i++ % uint32_t(n)]->push_in_mbox(i);
        }
    });
    for (auto& t : ts) t.join();
    done = true;
    mbox.join();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    uint32_t got = vm::read32(line);
    FILETIME c0, e0, k0, u0;
    GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k0, &u0);
    auto ft = [](FILETIME f) { return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 10000.0; };
    std::printf("park ring cpu: user %.0f ms kernel %.0f ms over %.0f ms wall\n", ft(u0), ft(k0), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    std::printf("park ring: %d threads, counter %u/%u, %.1f ms (%.2f us/handoff), waits %llu, slow waits %llu, max wait %llu us, retries %llu\n", n, got, total, ms, ms * 1000.0 / total,
                (unsigned long long)waits.load(), (unsigned long long)slow.load(), (unsigned long long)max_us.load(), (unsigned long long)retries.load());
    return got == total && slow == 0 ? 0 : 1;
}

}

int main(int argc, char** argv) {
    vm::init();
    const uint32_t base = 0x30000000;
    vm::map(base, 0x01000000);
    for (uint32_t i = 0; i < 0x01000000; i += 4) vm::write32(base + i, i * 2654435761u);
    if (argc > 1 && std::string(argv[1]) == "park") {
        int r = park_ring(base, argc > 2 ? std::atoi(argv[2]) : 20000, argc > 3 ? std::atoi(argv[3]) : 6, argc > 4 ? std::atoi(argv[4]) : 0);
        std::fflush(stdout);
        std::_Exit(r);
    }
    if (argc > 1 && std::string(argv[1]) == "stress") {
        int r = stress(base, argc > 2 ? std::atoi(argv[2]) : 200000, argc > 3 && std::string(argv[3]) != "far", argc > 3 && std::string(argv[3]) == "wrap");
        std::fflush(stdout);
        std::_Exit(r);
    }
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
        measure("baseline: memcpy 0x4000 to guest", 20000, [](int) {}, [&](int i) { std::memcpy(vm::ptr(base + 0xB80000 + (i & 7) * 0x4000), th.ls, 0x4000); });
        measure("baseline: memcpy 0x4000 to guest + mfence", 20000, [](int) {}, [&](int i) { std::memcpy(vm::ptr(base + 0xB80000 + (i & 7) * 0x4000), th.ls, 0x4000); _mm_mfence(); });
        measure("baseline: rsv::store_range 0x4000 same dst", 20000, [](int) {}, [&](int i) { rsv::store_range(base + 0xB80000, th.ls, 0x4000); });
        measure("baseline: rsv::store_range 0x4000", 20000, [](int) {}, [&](int i) { rsv::store_range(base + 0xB80000 + (i & 7) * 0x4000, th.ls, 0x4000); });
        measure("baseline: rsv::copy_range_if_changed 0x4000 same", 20000, [](int) {}, [&](int) { uint32_t lo = ~0u, hi = 0; g_sink += rsv::copy_range_if_changed(base, th.ls + 0x30000, 0x4000, lo, hi); });
        measure("baseline: rsv::copy_range_prefetch 0x4000 same", 20000, [](int) {}, [&](int) { uint32_t lo = ~0u, hi = 0; g_sink += rsv::copy_range_prefetch(base, th.ls + 0x30000, 0x4000, lo, hi); });
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
