#pragma once

#include <cstdint>
#include <string>

namespace loader {

struct ProcessInfo {
    uint32_t sdk_version = 0x430001;
    int32_t primary_prio = 1000;
    uint32_t primary_stacksize = 0x10000;
    uint32_t malloc_pagesize = 0x100000;
    uint32_t tls_vaddr = 0;
    uint32_t tls_filesz = 0;
    uint32_t tls_memsz = 0;
    uint32_t entry_opd = 0;
};

extern ProcessInfo g_process;
extern std::string g_root;

int find_image(const char* name);
int find_image_by_path(const std::string& path);
bool load_image(int index);
bool image_loaded(int index);
std::string host_path(const std::string& relative);

}
