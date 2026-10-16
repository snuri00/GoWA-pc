#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace vfs {

void init();
std::string host_path(const std::string& guest);
std::string disc_root();
std::string patch_root();

struct Psf {
    std::map<std::string, std::string> strings;
    std::map<std::string, uint32_t> ints;
    bool load(const std::string& host_file);
    std::string str(const std::string& key) const;
    uint32_t num(const std::string& key, uint32_t def = 0) const;
};

const Psf& disc_sfo();
const Psf& patch_sfo();

}
