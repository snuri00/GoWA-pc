#include "images.h"

#include <fstream>
#include <iterator>

namespace scimg {

std::vector<Image>& images() {
    static std::vector<Image> v = {
        {"mp3pack", 0x7e1c00, 0, 0, {}},
        {"synth_submix", 0x7aae00, 0, 0, {}},
        {"aux_master", 0x7bfc80, 0, 0, {}},
        {"mp3_sampler", 0x7ff280, 0, 0, {}},
    };
    return v;
}

bool load_images(const std::string& elf_path) {
    std::ifstream f(elf_path, std::ios::binary);
    std::vector<uint8_t> elf(std::istreambuf_iterator<char>(f), {});
    for (Image& im : images()) {
        uint32_t off = im.ea - 0x10000;
        if (elf.size() < off + 0x30) return false;
        im.entry = be32(elf.data() + off + 16);
        im.fsize = be32(elf.data() + off + 20);
        if (elf.size() < off + im.fsize) return false;
        im.bytes.assign(elf.begin() + off, elf.begin() + off + im.fsize);
    }
    return true;
}

void Tracker::check_line(const uint8_t* ls, uint32_t lsa) {
    const std::vector<Image>& im = images();
    for (int i = 0; i < int(im.size()); i++) {
        if (std::memcmp(ls + lsa, im[i].bytes.data(), 0x30) != 0) continue;
        for (size_t k = 0; k < inst.size(); k++)
            if (inst[k].base == lsa) {
                inst.erase(inst.begin() + k);
                break;
            }
        inst.push_back({i, lsa});
    }
}

void Tracker::scan_all(const uint8_t* ls) {
    inst.clear();
    for (uint32_t a = 0; a < spu::LS_SIZE; a += 128) check_line(ls, a);
}

void Tracker::on_lines(const edgecap::Event& e, const uint8_t* ls) {
    for (auto& [l, d] : e.lines) check_line(ls, l & 0x3ff80);
}

bool Tracker::loaded(const uint8_t* ls, int img, uint32_t base) const {
    return std::memcmp(ls + base, images()[img].bytes.data(), 0x30) == 0;
}

int Tracker::find(const uint8_t* ls, uint32_t pc, uint32_t* base) const {
    const std::vector<Image>& im = images();
    for (size_t k = inst.size(); k-- > 0;) {
        const Instance& in = inst[k];
        if (pc < in.base + 0x30 || pc >= in.base + im[in.img].fsize) continue;
        if (std::memcmp(ls + pc, im[in.img].bytes.data() + (pc - in.base), 4) != 0) continue;
        if (base) *base = in.base;
        return in.img;
    }
    return -1;
}

}
