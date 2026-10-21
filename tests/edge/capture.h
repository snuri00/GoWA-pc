#pragma once

#include "spu/context.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace edgecap {

struct Event {
    int32_t kind = 0;
    uint32_t ch = 0;
    uint32_t value = 0;
    uint32_t pc = 0;
    uint32_t srr0 = 0;
    uint8_t ie = 0;
    uint8_t halted = 0;
    uint8_t result = 0;
    std::vector<std::pair<uint32_t, const uint8_t*>> lines;
};

struct Capture {
    std::vector<uint8_t> raw;
    uint32_t slot = 0;
    uint32_t insns = 0;
    uint32_t pc0 = 0, srr00 = 0, ie0 = 0;
    const uint8_t* gpr0 = nullptr;
    const uint8_t* ls0 = nullptr;
    std::vector<Event> events;
    uint32_t pc1 = 0, srr01 = 0, ie1 = 0;
    const uint8_t* gpr1 = nullptr;
    const uint8_t* ls1 = nullptr;
    bool load(const std::string& path);
};

struct Dma {
    uint32_t cmd = 0;
    uint32_t lsa = 0;
    uint32_t eah = 0;
    uint32_t ea = 0;
    uint32_t size = 0;
    uint32_t tag = 0;
    uint32_t pc = 0;
};

class Machine : public spu::Host {
public:
    const Capture& cap;
    spu::Context c{};
    std::vector<uint8_t> ls;
    size_t pos = 0;
    uint64_t n = 0;
    std::string error;
    uint32_t mfc[6] = {};
    std::function<void(const Dma&, bool after)> on_dma;
    std::function<void(uint32_t ch, uint32_t value)> on_write;

    explicit Machine(const Capture& cp);
    void reset();
    bool step();
    void skip(uint64_t k);
    bool done() const { return n >= cap.insns || c.halted; }
    bool verify_final(std::string& diff) const;

    const Event* next(int kind, uint32_t ch, uint32_t value, bool check);
    uint32_t read_channel(uint32_t ch) override;
    void write_channel(uint32_t ch, uint32_t value) override;
    uint32_t channel_count(uint32_t ch) override;
    bool stop_and_signal(uint32_t code) override;
    void halt(uint32_t pc, uint32_t word) override;
    bool interrupt_pending() override { return pos < cap.events.size() && cap.events[pos].kind == 5; }
};

}
