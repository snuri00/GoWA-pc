#pragma once

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

struct SpuThread : Host {
    uint32_t id = 0;
    uint32_t slot = 0;
    uint32_t order = 0;
    std::string name;
    Group* group = nullptr;
    Context ctx{};
    uint8_t* ls = nullptr;
    uint32_t image_entry = 0;
    std::vector<std::array<uint32_t, 4>> segments;
    uint64_t args[4] = {};
    uint32_t option = 0;

    std::mutex m;
    std::condition_variable cv;
    std::atomic<bool> stop_flag{false};
    std::atomic<uint64_t> insns{0};
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
    bool run_list(uint32_t tag, uint32_t cmd, uint32_t lsa, uint32_t list, uint32_t size);
    uint8_t* translate(uint32_t ea, uint32_t size, bool write, bool& handled, uint32_t lsa);
    template <class Pred>
    bool block(std::unique_lock<std::mutex>& lk, Pred pred);
    uint32_t pop_out_mbox();
};

void register_thread(SpuThread* t);
void status_dump();

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
