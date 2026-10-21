#pragma once

#include "capture.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace scimg {

struct Image {
    const char* name;
    uint32_t ea;
    uint32_t fsize;
    uint32_t entry;
    std::vector<uint8_t> bytes;
};

struct Instance {
    int img = -1;
    uint32_t base = 0;
};

inline uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

std::vector<Image>& images();
bool load_images(const std::string& elf_path);

class Tracker {
public:
    std::vector<Instance> inst;
    void scan_all(const uint8_t* ls);
    void on_lines(const edgecap::Event& e, const uint8_t* ls);
    int find(const uint8_t* ls, uint32_t pc, uint32_t* base) const;
    bool loaded(const uint8_t* ls, int img, uint32_t base) const;

private:
    void check_line(const uint8_t* ls, uint32_t lsa);
};

class TrackedMachine : public edgecap::Machine {
public:
    Tracker tr;
    explicit TrackedMachine(const edgecap::Capture& cp) : edgecap::Machine(cp) { tr.scan_all(ls.data()); }
    bool tstep() {
        size_t p0 = pos;
        bool ok = step();
        for (size_t i = p0; i < pos; i++) tr.on_lines(cap.events[i], ls.data());
        return ok;
    }
    void tskip(uint64_t k) {
        size_t p0 = pos;
        skip(k);
        for (size_t i = p0; i < pos; i++) tr.on_lines(cap.events[i], ls.data());
    }
};

}
