#pragma once

#include <cstdint>

namespace renjob {

struct ChanHost {
    bool abort = false;
    virtual uint32_t rd(uint32_t ch) = 0;
    virtual void wr(uint32_t ch, uint32_t value) = 0;
    virtual uint32_t cnt(uint32_t ch) = 0;

protected:
    ~ChanHost() = default;
};

}
