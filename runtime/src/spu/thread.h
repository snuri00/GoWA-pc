#pragma once

#include "runtime/reservation.h"

#include "spu/context.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace spu {

struct Group;

extern int g_edge_direct;
extern std::atomic<uint64_t> g_edge_merge_hits;

struct SpuThread : Host {
    uint32_t id = 0;
    uint32_t slot = 0;
    uint32_t order = 0;
    std::string name;
    Group* group = nullptr;
    Context ctx{};
    uint8_t* ls = nullptr;
    std::vector<uint32_t> region_valid;
    struct DmaSnap {
        uint32_t ea = 0;
        uint32_t size = 0;
        uint32_t lsa = 0;
        std::vector<uint8_t> data;
    };
    std::array<DmaSnap, 64> dma_snaps{};
    std::array<uint64_t, 0x40000 / 128> snap_line{};
    void clear_snap(uint32_t k);
    std::unique_ptr<uint32_t[]> line_src{new uint32_t[0x40000 / 128]()};
    uint32_t cur_src = 0;
    uint64_t src_insn_mark = 0;
    uint32_t dma_snap_next = 0;
    void remember_get(uint32_t ea, uint32_t size, uint32_t lsa);
    void log_watched_dma(const char* what, uint32_t cmd, uint32_t ea, uint32_t lsa, uint32_t size, const uint8_t* data);
    void forget_ls(uint32_t lsa, uint32_t size);
    bool merge_put(uint32_t ea, uint32_t size, uint32_t lsa);
    bool merge_hit = false;
    uint64_t edge_direct_puts = 0;
    uint64_t edge_nosnap_gets = 0;
    std::atomic<uint32_t> ls_epoch{1};
    std::atomic<uint32_t> ls_all_epoch{1};
    std::unique_ptr<std::atomic<uint32_t>[]> line_epoch{new std::atomic<uint32_t>[0x40000 / 128]()};
    std::atomic<bool> cap_on{false};
    std::atomic_flag cap_lock = ATOMIC_FLAG_INIT;
    std::vector<uint32_t> cap_lines;
    void note_cap_write(uint32_t lsa, uint32_t size) {
        while (cap_lock.test_and_set(std::memory_order_acquire)) {
        }
        uint32_t first = (lsa & 0x3FFFF) >> 7, last = ((lsa & 0x3FFFF) + size - 1) >> 7;
        for (uint32_t l = first; l <= last && l < 0x40000 / 128; l++) cap_lines.push_back(l);
        cap_lock.clear(std::memory_order_release);
    }
    void mark_ls(uint32_t lsa, uint32_t size) {
        if (!size) return;
        if (cap_on.load(std::memory_order_relaxed)) note_cap_write(lsa, size);
        uint32_t e = ls_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
        uint32_t first = (lsa & 0x3FFFF) >> 7, last = ((lsa & 0x3FFFF) + size - 1) >> 7;
        for (uint32_t l = first; l <= last && l < 0x40000 / 128; l++) line_epoch[l].store(e, std::memory_order_release);
    }
    int park_slot = rsv::park_alloc();
    void wake_rsv_waiter() {
        uint32_t r = raddr;
        if (r) rsv::wake(r);
        rsv::park_kick(park_slot);
    }
    void mark_all_ls() {
        if (cap_on.load(std::memory_order_relaxed)) note_cap_write(0, 0x40000);
        uint32_t e = ls_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
        ls_all_epoch.store(e, std::memory_order_release);
    }
    bool ls_fresh(uint32_t pc, uint32_t words, uint32_t validated) const {
        if (!validated || ls_all_epoch.load(std::memory_order_acquire) > validated) return false;
        uint32_t first = pc >> 7, last = (pc + words * 4 - 1) >> 7;
        for (uint32_t l = first; l <= last && l < 0x40000 / 128; l++) {
            if (line_epoch[l].load(std::memory_order_acquire) > validated) return false;
        }
        return true;
    }
    uint32_t image_entry = 0;
    std::vector<std::array<uint32_t, 4>> segments;
    uint64_t args[4] = {};
    uint32_t option = 0;

    std::mutex m;
    std::condition_variable cv;
    std::atomic<bool> stop_flag{false};
    std::atomic<uint64_t> insns{0};
    std::atomic<uint32_t> waiting{0};
    uint32_t pc_samples[256] = {};
    std::atomic<uint32_t> pc_sample_pos{0};
    bool running = false;
    uint32_t exit_status = 0;
    bool exited_by_thread_exit = false;

    std::deque<uint32_t> in_mbox;
    bool out_mbox_full = false;
    uint32_t out_mbox = 0;
    bool out_intr_full = false;
    uint32_t out_intr = 0;
    uint32_t snr[2] = {};
    bool snr_count[2] = {};
    uint32_t snr_config = 0;

    uint32_t events = 0;
    uint32_t event_mask = 0;
    bool event_count = false;
    uint64_t dec_start = 0;
    uint32_t dec_value = 0;
    bool dec_frozen = false;
    bool irq_taken = false;
    void* native = nullptr;

    uint32_t mfc_lsa = 0, mfc_eal = 0, mfc_size = 0, mfc_tag = 0;
    uint32_t tag_mask = 0;
    uint32_t tag_stat = 0;
    bool tag_stat_count = false;
    uint32_t stall_stat = 0;
    bool stall_stat_count = false;
    uint32_t atomic_stat = 0;
    bool atomic_stat_count = false;
    struct ListState {
        bool active = false;
        uint32_t cmd = 0, lsa = 0, list = 0, remaining = 0;
    } stalled[32];

    uint32_t raddr = 0;
    uint64_t rtime = 0;
    alignas(16) uint8_t rdata[128] = {};

    uint32_t spup[64] = {};
    std::array<std::pair<uint32_t, uint32_t>, 32> spuq{};

    SpuThread();
    ~SpuThread() override;

    uint32_t read_channel(uint32_t ch) override;
    void write_channel(uint32_t ch, uint32_t value) override;
    uint32_t channel_count(uint32_t ch) override;
    bool stop_and_signal(uint32_t code) override;
    void halt(uint32_t pc, uint32_t word) override;
    bool interrupt_pending() override;

    void push_snr(uint32_t number, uint32_t value);
    void push_in_mbox(uint32_t value);
    void deploy();
    void start();
    void run();

private:
    void set_events(uint32_t bits);
    void check_interrupt();
    void collect_events();
    uint32_t dec_now();
    void process_mfc(uint32_t cmd);
    void dma(uint32_t cmd, uint32_t lsa, uint32_t ea, uint32_t size);
    bool dma_direct(uint32_t cmd, uint32_t lsa, uint32_t ea, uint32_t size);
    bool run_list(uint32_t tag, uint32_t cmd, uint32_t lsa, uint32_t list, uint32_t size);
    uint8_t* translate(uint32_t ea, uint32_t size, bool write, bool& handled, uint32_t lsa);
    template <class Pred>
    bool block(std::unique_lock<std::mutex>& lk, Pred pred);
    uint32_t pop_out_mbox();
};

void register_thread(SpuThread* t);
void status_dump();
void request_capture(const std::string& dir);
void lowfps_tick(uint64_t flips);

struct Group {
    uint32_t id = 0;
    std::string name;
    uint32_t num = 0;
    int32_t prio = 0;
    int32_t type = 0;
    std::mutex m;
    std::condition_variable cv;
    uint32_t state = 0;
    uint32_t init = 0;
    std::vector<std::shared_ptr<SpuThread>> threads;
    uint32_t running = 0;
    uint32_t join_state = 0;
    uint32_t exit_status = 0;
    bool joining = false;
    uint32_t run_queue = 0;
    uint32_t exception_queue = 0;
    uint32_t system_queue = 0;

    SpuThread* by_slot(uint32_t slot);
    void thread_stopped(SpuThread& t);
    void stop_all();
};

}
