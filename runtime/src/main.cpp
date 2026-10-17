#include "runtime/image.h"
#include "runtime/loader.h"
#include "runtime/log.h"
#include "runtime/trace.h"
#include "runtime/memory.h"
#include "gpu/presenter.h"
#include "runtime/thread.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include "runtime/vfs.h"

namespace spu {
void status_dump();
}

namespace lv2 {
void rsx_status();
}

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

uint32_t push_strings(uint32_t& sp, const char* const* list, int count) {
    uint32_t ptrs = 0;
    std::vector<uint32_t> addrs;
    for (int i = 0; i < count; i++) {
        uint32_t len = static_cast<uint32_t>(std::strlen(list[i])) + 1;
        sp -= (len + 15) & ~15u;
        std::memcpy(vm::ptr(sp), list[i], len);
        addrs.push_back(sp);
    }
    sp -= static_cast<uint32_t>((addrs.size() + 1) * 8 + 15) & ~15u;
    ptrs = sp;
    for (size_t i = 0; i < addrs.size(); i++) vm::write64(ptrs + static_cast<uint32_t>(i) * 8, addrs[i]);
    vm::write64(ptrs + static_cast<uint32_t>(addrs.size()) * 8, 0);
    return ptrs;
}

}

namespace crash {
void install();
void arm_watch();
void rearm_watch();
}

int main(int argc, char** argv) {
    crash::install();
    trace::init();
    timeBeginPeriod(1);
    gpu::init();
    if (const char* root = std::getenv("GOWA_ROOT")) loader::g_root = root;
    if (const char* lvl = std::getenv("GOWA_LOG")) {
        if (!std::strcmp(lvl, "trace")) logging::set_level(logging::Level::Trace);
        else if (!std::strcmp(lvl, "debug")) logging::set_level(logging::Level::Debug);
    }
    LOG_INFO("main", "GOWA-pc: %u images, %u functions, %u imports, %u exports, root %s", image::image_count, image::function_count, image::import_count, image::export_count, loader::g_root.c_str());
    if (!vm::init()) return 1;
    int game = loader::find_image("gowa");
    int lv2 = loader::find_image("liblv2");
    if (game < 0 || lv2 < 0 || !loader::load_image(game) || !loader::load_image(lv2)) return 1;
    const loader::ProcessInfo& pi = loader::g_process;
    LOG_INFO("main", "sdk 0x%x prio %d stack 0x%x malloc page 0x%x tls 0x%x/0x%x/0x%x", pi.sdk_version, pi.primary_prio, pi.primary_stacksize, pi.malloc_pagesize, pi.tls_vaddr, pi.tls_filesz, pi.tls_memsz);
    uint32_t stack_size = pi.primary_stacksize;
    switch (stack_size) {
    case 0x10: stack_size = 32 * 1024; break;
    case 0x20: stack_size = 64 * 1024; break;
    case 0x30: stack_size = 96 * 1024; break;
    case 0x40: stack_size = 128 * 1024; break;
    case 0x50: stack_size = 256 * 1024; break;
    case 0x60: stack_size = 512 * 1024; break;
    case 0x70: stack_size = 1024 * 1024; break;
    default: stack_size = std::max<uint32_t>(0x10000, (stack_size + 0xFFF) & ~0xFFFu); break;
    }
    auto main_thread = guest::create("main_thread", image::images[lv2].entry, 0, 0, pi.primary_prio, stack_size, 0, true);
    if (!main_thread) return 1;
    uint32_t sp = main_thread->stack_addr + main_thread->stack_size;
    std::string self_path = vfs::patch_sfo().strings.empty() ? "/dev_bdvd/PS3_GAME/USRDIR/GOWA.SELF" : "/dev_hdd0/game/BCES01741/USRDIR/GOWA.SELF";
    std::vector<std::string> arg_list = {self_path};
    if (const char* extra = std::getenv("GOWA_ARGS")) {
        std::string e = extra;
        size_t pos = 0;
        while (pos < e.size()) {
            size_t end = e.find(' ', pos);
            if (end == std::string::npos) end = e.size();
            if (end > pos) arg_list.push_back(e.substr(pos, end - pos));
            pos = end + 1;
        }
    }
    std::vector<const char*> args;
    for (const std::string& a : arg_list) args.push_back(a.c_str());
    LOG_INFO("main", "argc %zu%s%s", args.size(), args.size() > 1 ? " extra: " : "", std::getenv("GOWA_ARGS") ? std::getenv("GOWA_ARGS") : "");
    uint32_t argv_addr = push_strings(sp, args.data(), static_cast<int>(args.size()));
    uint32_t envp_addr = push_strings(sp, nullptr, 0);
    ppu::Context& c = main_thread->ctx;
    c.gpr[1] = (sp - 0x70) & ~15u;
    c.gpr[3] = args.size();
    c.gpr[4] = argv_addr;
    c.gpr[5] = envp_addr;
    c.gpr[6] = 0;
    c.gpr[7] = main_thread->id;
    c.gpr[8] = pi.tls_vaddr;
    c.gpr[9] = pi.tls_filesz;
    c.gpr[10] = pi.tls_memsz;
    c.gpr[11] = pi.entry_opd;
    c.gpr[12] = pi.malloc_pagesize;
    int watch_delay = std::getenv("GOWA_TRAP_AT") ? std::atoi(std::getenv("GOWA_TRAP_AT")) : std::getenv("GOWA_WATCH_DELAY") ? std::atoi(std::getenv("GOWA_WATCH_DELAY")) : 0;
    if (!watch_delay) crash::arm_watch();
    guest::start(main_thread);
    bool status = std::getenv("GOWA_STATUS") != nullptr;
    int memdump_at = std::getenv("GOWA_MEMDUMP_AT") ? std::atoi(std::getenv("GOWA_MEMDUMP_AT")) : -1;
    for (int tick = 0;; tick++) {
        if (watch_delay && tick == watch_delay) crash::arm_watch();
        else if (tick > watch_delay) crash::rearm_watch();
        if (tick == memdump_at) trace::dump_memory("memdump.bin");
        if (status && tick % 3 == 2) {
            spu::status_dump();
            guest::status_dump();
            lv2::rsx_status();
        }
        std::unique_lock lk(main_thread->lock);
        if (main_thread->cv.wait_for(lk, std::chrono::seconds(1), [&] { return main_thread->exited; })) break;
    }
    LOG_INFO("main", "main thread finished");
    trace::shutdown();
    return 0;
}
