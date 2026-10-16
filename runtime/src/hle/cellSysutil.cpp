#include "common.h"
#include "runtime/vfs.h"

#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>

namespace hle {

namespace {

struct Callback {
    uint32_t func = 0;
    uint32_t userdata = 0;
};

std::mutex g_lock;
Callback g_callbacks[4];
std::deque<std::function<void(ppu::Context&)>> g_pending;

void post(std::function<void(ppu::Context&)> fn) {
    std::lock_guard lk(g_lock);
    g_pending.push_back(std::move(fn));
}

}

HLE_FUNC(cellSysutil, cellSysutilRegisterCallback) {
    uint32_t slot = a32(ctx, 0);
    if (slot > 3) return ret(ctx, static_cast<int32_t>(0x8002b101));
    std::lock_guard lk(g_lock);
    g_callbacks[slot] = {a32(ctx, 1), a32(ctx, 2)};
    ret(ctx, 0);
}

HLE_FUNC(cellSysutil, cellSysutilUnregisterCallback) {
    uint32_t slot = a32(ctx, 0);
    if (slot > 3) return ret(ctx, static_cast<int32_t>(0x8002b101));
    std::lock_guard lk(g_lock);
    g_callbacks[slot] = {};
    ret(ctx, 0);
}

HLE_FUNC(cellSysutil, cellSysutilCheckCallback) {
    for (;;) {
        std::function<void(ppu::Context&)> fn;
        {
            std::lock_guard lk(g_lock);
            if (g_pending.empty()) break;
            fn = std::move(g_pending.front());
            g_pending.pop_front();
        }
        fn(ctx);
    }
    ret(ctx, 0);
}

HLE_FUNC(cellSysutil, cellSysutilGetSystemParamInt) {
    uint32_t id = a32(ctx, 0), out = a32(ctx, 1);
    uint32_t v = 0;
    switch (id) {
    case 0x111: v = 1; break;
    case 0x112: v = 1; break;
    case 0x114: v = 1; break;
    case 0x115: v = 1; break;
    case 0x116: v = 0; break;
    case 0x117: v = 0; break;
    case 0x121: v = 9; break;
    case 0x123: v = 0; break;
    case 0x141: v = 0; break;
    case 0x151: v = 1; break;
    case 0x152: v = 1; break;
    case 0x156: v = 0; break;
    default: LOG_WARN("cellSysutil", "unknown system param int 0x%x", id); break;
    }
    if (out) vm::write32(out, v);
    ret(ctx, 0);
}

HLE_FUNC(cellSysutil, cellSysutilGetSystemParamString) {
    uint32_t id = a32(ctx, 0), buf = a32(ctx, 1), size = a32(ctx, 2);
    put_str(buf, id == 0x131 ? "Kratos" : "", size);
    ret(ctx, 0);
}

HLE_FUNC(cellSysutil, cellVideoOutGetState) {
    uint32_t out = a32(ctx, 2);
    if (!out) return ret(ctx, static_cast<int32_t>(0x8002b222));
    std::memset(vm::ptr(out), 0, 16);
    if (a32(ctx, 0) != 0) {
        vm::ptr(out)[0] = 2;
        return ret(ctx, 0);
    }
    uint8_t* p = vm::ptr(out);
    p[0] = 0;
    p[1] = 1;
    p[8] = 2;
    p[9] = 1;
    p[10] = 0;
    p[11] = 2;
    vm::write16(out + 14, 1);
    ret(ctx, 0);
}

HLE_FUNC(cellSysutil, cellVideoOutGetResolution) {
    uint32_t id = a32(ctx, 0), out = a32(ctx, 1);
    uint16_t w = 0, h = 0;
    switch (id) {
    case 1: w = 1920; h = 1080; break;
    case 2: w = 1280; h = 720; break;
    case 4: w = 720; h = 480; break;
    case 5: w = 720; h = 576; break;
    default: return ret(ctx, static_cast<int32_t>(0x8002b224));
    }
    vm::write16(out, w);
    vm::write16(out + 2, h);
    ret(ctx, 0);
}

HLE_FUNC(cellSysutil, cellVideoOutGetResolutionAvailability) {
    uint32_t id = a32(ctx, 1);
    ret(ctx, (id == 1 || id == 2) ? 1 : 0);
}

HLE_FUNC(cellSysutil, cellVideoOutConfigure) {
    uint32_t cfg = a32(ctx, 1);
    if (cfg) LOG_INFO("cellSysutil", "cellVideoOutConfigure res %u format %u aspect %u pitch %u", vm::ptr(cfg)[0], vm::ptr(cfg)[1], vm::ptr(cfg)[2], vm::read32(cfg + 12));
    ret(ctx, 0);
}

HLE_FUNC(cellSysutil, cellAudioOutGetSoundAvailability) {
    uint32_t type = a32(ctx, 1);
    ret(ctx, type == 0 ? 2 : 0);
}

HLE_FUNC(cellSysutil, cellAudioOutConfigure) {
    ret(ctx, 0);
}

HLE_FUNC(cellSysutil, cellSysCacheMount) {
    uint32_t param = a32(ctx, 0);
    std::string id = gstr(param, 32);
    std::string guest = "/dev_hdd1/caches/" + (id.empty() ? std::string("BCES01741") : id);
    std::error_code ec;
    std::filesystem::create_directories(vfs::host_path(guest), ec);
    put_str(param + 32, guest, 1024);
    LOG_INFO("cellSysutil", "cellSysCacheMount('%s') -> %s", id.c_str(), guest.c_str());
    ret(ctx, 0);
}

HLE_FUNC(cellSysutil, cellSysCacheClear) {
    ret(ctx, 0);
}

HLE_FUNC(cellSysutil, cellMsgDialogOpen2) {
    uint32_t type = a32(ctx, 0);
    std::string msg = gstr(a32(ctx, 1));
    uint32_t cb = a32(ctx, 2), user = a32(ctx, 3);
    LOG_WARN("cellSysutil", "cellMsgDialogOpen2(0x%x) '%s'", type, msg.c_str());
    if (cb) {
        uint32_t button = ((type >> 4) & 7) == 1 ? 1 : 3;
        post([cb, user, button](ppu::Context& c) { call_guest(c, cb, button, user); });
    }
    ret(ctx, 0);
}

HLE_FUNC(cellSysutil, cellSaveDataAutoLoad2) {
    LOG_WARN("cellSysutil", "cellSaveDataAutoLoad2('%s') -> NODATA", gstr(a32(ctx, 1)).c_str());
    ret(ctx, static_cast<int32_t>(0x8002b40b));
}

HLE_FUNC(cellSysutil, cellSaveDataListAutoLoad) {
    ret(ctx, static_cast<int32_t>(0x8002b40b));
}

HLE_FUNC(cellSysutil, cellSaveDataListLoad2) {
    ret(ctx, static_cast<int32_t>(0x8002b40b));
}

HLE_FUNC(cellSysutil, cellSaveDataAutoSave2) {
    LOG_WARN("cellSysutil", "cellSaveDataAutoSave2('%s') not implemented", gstr(a32(ctx, 1)).c_str());
    ret(ctx, 0);
}

HLE_FUNC(cellSysutil, cellSaveDataDelete2) {
    ret(ctx, 0);
}

HLE_FUNC(cellSysutil, cellSaveDataFixedDelete) {
    ret(ctx, 0);
}

HLE_FUNC(cellSysutilAvconfExt, cellVideoOutGetScreenSize) {
    if (uint32_t p = a32(ctx, 1)) vm::write32(p, 0);
    ret(ctx, static_cast<int32_t>(0x8002b226));
}

HLE_FUNC(cellSysutilAvconfExt, cellVideoOutSetGamma) {
    ret(ctx, 0);
}

}
