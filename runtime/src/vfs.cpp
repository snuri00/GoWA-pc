#include "runtime/vfs.h"
#include "runtime/log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

namespace vfs {

namespace {

struct Mount {
    std::string guest;
    std::string host;
};

std::vector<Mount> g_mounts;
std::string g_disc;
std::string g_patch;
Psf g_disc_sfo;
Psf g_patch_sfo;
std::once_flag g_once;

std::string env_or(const char* name, const char* def) {
    const char* v = std::getenv(name);
    return v && *v ? v : def;
}

std::string lower(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

}

void init() {
    std::call_once(g_once, [] {
        std::string rpcs3 = env_or("GOWA_RPCS3_DIR", "D:/Games/rpcs3");
        std::string local = env_or("GOWA_VFS_DIR", "vfs");
        g_disc = env_or("GOWA_DISC_DIR", (rpcs3 + "/games/BCES01741").c_str());
        std::string rpcs3_hdd0 = rpcs3 + "/dev_hdd0";
        std::string hdd0 = env_or("GOWA_HDD0_DIR", (local + "/dev_hdd0").c_str());
        std::string hdd1 = env_or("GOWA_HDD1_DIR", (local + "/dev_hdd1").c_str());
        std::string usb = local + "/dev_usb000";
        std::string flash = env_or("GOWA_FLASH_DIR", (rpcs3 + "/dev_flash").c_str());
        for (const std::string& d : {hdd0 + "/game", hdd0 + "/home/00000001/savedata", hdd0 + "/home/00000001/trophy", hdd1, usb}) {
            std::error_code ec;
            std::filesystem::create_directories(d, ec);
        }
        g_patch = env_or("GOWA_PATCH_DIR", (rpcs3_hdd0 + "/game/BCES01741").c_str());
        g_mounts = {
            {"/dev_bdvd", g_disc},
            {"/dev_hdd0/game/bces01741", g_patch},
            {"/dev_hdd0", hdd0},
            {"/dev_hdd1", hdd1},
            {"/dev_flash", flash},
            {"/dev_usb000", usb},
            {"/app_home", g_disc + "/PS3_GAME/USRDIR"},
            {"/host_root", "."},
        };
        g_disc_sfo.load(g_disc + "/PS3_GAME/PARAM.SFO");
        g_patch_sfo.load(g_patch + "/PARAM.SFO");
        LOG_INFO("vfs", "disc %s (%s %s), patch %s (%s)", g_disc.c_str(), g_disc_sfo.str("TITLE_ID").c_str(), g_disc_sfo.str("APP_VER").c_str(), g_patch.c_str(), g_patch_sfo.str("APP_VER").c_str());
    });
}

std::string host_path(const std::string& guest) {
    init();
    std::string g = guest;
    while (g.find("//") != std::string::npos) g.replace(g.find("//"), 2, "/");
    for (const auto& m : g_mounts) {
        if (lower(g.substr(0, m.guest.size())) == m.guest && (g.size() == m.guest.size() || g[m.guest.size()] == '/')) {
            return m.host + g.substr(m.guest.size());
        }
    }
    return {};
}

std::string disc_root() { init(); return g_disc; }
std::string patch_root() { init(); return g_patch; }
const Psf& disc_sfo() { init(); return g_disc_sfo; }
const Psf& patch_sfo() { init(); return g_patch_sfo; }

bool Psf::load(const std::string& host_file) {
    FILE* f = std::fopen(host_file.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> d;
    std::fseek(f, 0, SEEK_END);
    d.resize(static_cast<size_t>(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    size_t got = std::fread(d.data(), 1, d.size(), f);
    std::fclose(f);
    if (got < 20 || std::memcmp(d.data(), "\0PSF", 4) != 0) return false;
    auto le32 = [&](size_t o) { uint32_t v; std::memcpy(&v, &d[o], 4); return v; };
    auto le16 = [&](size_t o) { uint16_t v; std::memcpy(&v, &d[o], 2); return v; };
    uint32_t kt = le32(8), dt = le32(12), n = le32(16);
    for (uint32_t i = 0; i < n; i++) {
        size_t e = 20 + i * 16;
        std::string key = reinterpret_cast<const char*>(&d[kt + le16(e)]);
        uint16_t fmt = le16(e + 2);
        uint32_t len = le32(e + 4);
        uint32_t off = le32(e + 12);
        if (fmt == 0x0404) ints[key] = le32(dt + off);
        else strings[key] = std::string(reinterpret_cast<const char*>(&d[dt + off]), strnlen(reinterpret_cast<const char*>(&d[dt + off]), len));
    }
    return true;
}

std::string Psf::str(const std::string& key) const {
    auto it = strings.find(key);
    return it == strings.end() ? std::string() : it->second;
}

uint32_t Psf::num(const std::string& key, uint32_t def) const {
    auto it = ints.find(key);
    return it == ints.end() ? def : it->second;
}

}
