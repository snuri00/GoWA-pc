#include "runtime/loader.h"
#include "runtime/image.h"
#include "runtime/log.h"
#include "runtime/memory.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace loader {

ProcessInfo g_process;
std::string g_root = ".";

namespace {

std::mutex g_lock;
std::vector<bool> g_loaded;

uint64_t be64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return __builtin_bswap64(v); }
uint32_t be32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return __builtin_bswap32(v); }
uint16_t be16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return __builtin_bswap16(v); }

std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> v;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return v;
    std::fseek(f, 0, SEEK_END);
    v.resize(static_cast<size_t>(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
    std::fclose(f);
    return v;
}

}

std::string host_path(const std::string& relative) { return g_root + "/" + relative; }

int find_image(const char* name) {
    for (uint32_t i = 0; i < image::image_count; i++) {
        if (std::strcmp(image::images[i].name, name) == 0) return static_cast<int>(i);
    }
    return -1;
}

int find_image_by_path(const std::string& path) {
    std::string file = path.substr(path.find_last_of('/') + 1);
    std::string stem = file.substr(0, file.find('.'));
    return find_image(stem.c_str());
}

bool image_loaded(int index) {
    std::lock_guard lk(g_lock);
    return index >= 0 && static_cast<size_t>(index) < g_loaded.size() && g_loaded[index];
}

bool load_image(int index) {
    std::lock_guard lk(g_lock);
    if (g_loaded.size() < image::image_count) g_loaded.resize(image::image_count);
    if (g_loaded[index]) return true;
    const image::ImageEntry& im = image::images[index];
    auto data = read_file(host_path(im.file));
    if (data.size() < 64 || std::memcmp(data.data(), "\x7f" "ELF", 4) != 0) {
        LOG_ERROR("loader", "cannot read %s", host_path(im.file).c_str());
        return false;
    }
    uint64_t phoff = be64(&data[0x20]);
    uint16_t phes = be16(&data[0x36]);
    uint16_t phn = be16(&data[0x38]);
    for (uint16_t i = 0; i < phn; i++) {
        const uint8_t* ph = &data[phoff + uint64_t(i) * phes];
        uint32_t type = be32(ph);
        uint64_t off = be64(ph + 8);
        uint64_t vaddr = be64(ph + 16);
        uint64_t filesz = be64(ph + 32);
        uint64_t memsz = be64(ph + 40);
        if (type == 1 && memsz) {
            if (!vm::map(static_cast<uint32_t>(vaddr), static_cast<uint32_t>(memsz))) {
                LOG_ERROR("loader", "map failed 0x%llx+0x%llx", static_cast<unsigned long long>(vaddr), static_cast<unsigned long long>(memsz));
                return false;
            }
            std::memcpy(vm::ptr(static_cast<uint32_t>(vaddr)), &data[off], filesz);
            if (!(be32(ph + 4) & 2) && filesz == memsz) vm::mark_immutable(static_cast<uint32_t>(vaddr), static_cast<uint32_t>(filesz));
            std::memset(vm::ptr(static_cast<uint32_t>(vaddr + filesz)), 0, memsz - filesz);
        } else if (type == 7 && !im.prx) {
            g_process.tls_vaddr = static_cast<uint32_t>(vaddr);
            g_process.tls_filesz = static_cast<uint32_t>(filesz);
            g_process.tls_memsz = static_cast<uint32_t>(memsz);
        } else if (type == 0x60000001 && !im.prx) {
            const uint8_t* pp = &data[off];
            g_process.sdk_version = be32(pp + 12);
            g_process.primary_prio = static_cast<int32_t>(be32(pp + 16));
            g_process.primary_stacksize = be32(pp + 20);
            g_process.malloc_pagesize = be32(pp + 24);
        }
    }
    if (!im.prx) g_process.entry_opd = im.entry;
    g_loaded[index] = true;
    LOG_INFO("loader", "loaded %s (%s) text 0x%08x-0x%08x toc 0x%08x entry 0x%08x", im.name, im.file, im.text_lo, im.text_hi, im.toc, im.entry);
    return true;
}

}
