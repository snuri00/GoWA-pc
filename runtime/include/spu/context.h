#pragma once

#include <atomic>
#include "ppu/context.h"

#include <cstdint>

namespace spu {

using ppu::v128;

constexpr uint32_t LS_SIZE = 0x40000;
constexpr uint32_t LS_MASK = 0x3FFF0;
constexpr uint32_t PC_MASK = 0x3FFFC;

struct Context {
    v128 gpr[128];
    uint32_t pc = 0;
    uint32_t srr0 = 0;
    uint8_t* ls = nullptr;
    std::atomic<uint32_t>* line_epoch = nullptr;
    const std::atomic<uint32_t>* epoch = nullptr;
    const std::atomic<uint32_t>* all_epoch = nullptr;
    uint32_t* block_valid = nullptr;
    uint32_t executed = 0;
    bool interrupts_enabled = false;
    bool irq_check = false;
    bool halted = false;
};

inline uint32_t pref(const v128& v) { return v.u32[3]; }

class Host {
public:
    virtual ~Host() = default;
    virtual uint32_t read_channel(uint32_t ch) = 0;
    virtual void write_channel(uint32_t ch, uint32_t value) = 0;
    virtual uint32_t channel_count(uint32_t ch) = 0;
    virtual bool stop_and_signal(uint32_t code) = 0;
    virtual void halt(uint32_t pc, uint32_t word) = 0;
    virtual bool interrupt_pending() { return true; }
};

void interpret(Context& c, Host& host);
bool step(Context& c, Host& host);

}
