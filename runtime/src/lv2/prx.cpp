#include "object.h"
#include "runtime/image.h"
#include "runtime/loader.h"

#include <cstring>

namespace lv2 {

namespace {

enum class PrxState { Initialized, Starting, Started, Stopping, Stopped };

struct Prx : Object {
    std::mutex m;
    int image = -1;
    std::string path;
    PrxState state = PrxState::Initialized;
    uint32_t start_opd = 0;
    uint32_t stop_opd = 0;
};

constexpr int64_t CELL_PRX_ERROR_ERROR = static_cast<int32_t>(0x80011001);
constexpr int64_t CELL_PRX_ERROR_NOT_STARTED = static_cast<int32_t>(0x8001112E);
constexpr int64_t CELL_PRX_ERROR_ALREADY_STOPPED = static_cast<int32_t>(0x80011130);

uint32_t find_special_export(int image_index, uint32_t nid) {
    for (uint32_t i = 0; i < image::export_count; i++) {
        const image::ExportEntry& e = image::exports[i];
        if (static_cast<int>(e.image) == image_index && e.nid == nid && e.module[0] == 0) return e.opd;
    }
    return 0;
}

int64_t load(const std::string& path) {
    auto p = std::make_shared<Prx>();
    p->path = path;
    p->name = path.substr(path.find_last_of('/') + 1);
    int idx = loader::find_image_by_path(path);
    if (idx >= 0 && image::images[idx].prx) {
        if (!loader::load_image(idx)) return CELL_ENOEXEC;
        p->image = idx;
        p->start_opd = find_special_export(idx, 0xBC9A0086);
        p->stop_opd = find_special_export(idx, 0xAB779874);
    }
    uint32_t id = add_object(0x23000000, p);
    LOG_INFO("prx", "load '%s' -> 0x%x (%s, start 0x%08x)", path.c_str(), id, p->image >= 0 ? "LLE" : "HLE", p->start_opd);
    return id;
}

}

LV2_SYSCALL(480, _sys_prx_load_module) {
    uint32_t path = u32arg(ctx, 0);
    if (!path) return CELL_EFAULT;
    return load(reinterpret_cast<const char*>(vm::ptr(path)));
}

LV2_SYSCALL(497, _sys_prx_load_module_on_memcontainer) {
    uint32_t path = u32arg(ctx, 0);
    if (!path) return CELL_EFAULT;
    return load(reinterpret_cast<const char*>(vm::ptr(path)));
}

LV2_SYSCALL(465, _sys_prx_load_module_list) {
    int32_t count = s32arg(ctx, 0);
    uint32_t list = u32arg(ctx, 1);
    uint32_t ids = u32arg(ctx, 4);
    for (int32_t i = 0; i < count; i++) {
        uint32_t p = static_cast<uint32_t>(vm::read64(list + i * 8));
        int64_t r = load(reinterpret_cast<const char*>(vm::ptr(p)));
        if (r < 0) return r;
        vm::write32(ids + i * 4, static_cast<uint32_t>(r));
    }
    return CELL_OK;
}

LV2_SYSCALL(481, _sys_prx_start_module) {
    uint32_t id = u32arg(ctx, 0);
    uint32_t opt = u32arg(ctx, 2);
    if (!id || !opt) return CELL_EINVAL;
    auto p = get<Prx>(id);
    if (!p) return CELL_ESRCH;
    std::lock_guard lk(p->m);
    uint64_t cmd = vm::read64(opt + 8);
    switch (cmd & 0xF) {
    case 1:
        if (p->state != PrxState::Initialized) return CELL_PRX_ERROR_ERROR;
        p->state = PrxState::Starting;
        if (p->image < 0 && p->name == "libsysutil.sprx") {
            uint32_t area = vm::reserve_slot(0x10000000, 0x10000000);
            if (area) vm::map(area, 0x10000);
            LOG_INFO("prx", "libsysutil shared memory at 0x%08x", area);
        }
        vm::write64(opt + 16, p->start_opd ? p->start_opd : ~0ull);
        if (vm::read64(opt) != 0x20) vm::write64(opt + 32, ~0ull);
        return CELL_OK;
    case 2: {
        uint64_t res = vm::read64(opt + 24);
        if (res == 0) {
            p->state = PrxState::Started;
            LOG_INFO("prx", "started '%s'", p->name.c_str());
            return CELL_OK;
        }
        if (res & 0xFFFFFFFF) {
            p->state = PrxState::Stopped;
            return static_cast<int32_t>(res);
        }
        return CELL_OK;
    }
    default:
        return CELL_PRX_ERROR_ERROR;
    }
}

LV2_SYSCALL(482, _sys_prx_stop_module) {
    uint32_t id = u32arg(ctx, 0);
    uint32_t opt = u32arg(ctx, 2);
    auto p = get<Prx>(id);
    if (!p) return CELL_ESRCH;
    if (!opt) return CELL_EINVAL;
    std::lock_guard lk(p->m);
    uint64_t cmd = vm::read64(opt + 8);
    if ((cmd & 0xF) == 1) {
        if (p->state == PrxState::Initialized) return CELL_PRX_ERROR_NOT_STARTED;
        if (p->state == PrxState::Stopped) return CELL_PRX_ERROR_ALREADY_STOPPED;
        p->state = PrxState::Stopping;
        vm::write64(opt + 16, p->stop_opd ? p->stop_opd : ~0ull);
        if (vm::read64(opt) != 0x20) vm::write64(opt + 32, ~0ull);
        return CELL_OK;
    }
    if ((cmd & 0xF) == 2) {
        p->state = vm::read64(opt + 24) == 0 ? PrxState::Stopped : PrxState::Started;
        return CELL_OK;
    }
    return CELL_PRX_ERROR_ERROR;
}

LV2_SYSCALL(483, _sys_prx_unload_module) {
    if (!get<Prx>(u32arg(ctx, 0))) return CELL_ESRCH;
    remove_object(u32arg(ctx, 0));
    return CELL_OK;
}

LV2_SYSCALL(484, _sys_prx_register_module) {
    return CELL_OK;
}

LV2_SYSCALL(486, _sys_prx_register_library) {
    return CELL_OK;
}

LV2_SYSCALL(487, _sys_prx_unregister_library) {
    return CELL_OK;
}

LV2_SYSCALL(462, _sys_prx_unknown_462) {
    return CELL_OK;
}

}
