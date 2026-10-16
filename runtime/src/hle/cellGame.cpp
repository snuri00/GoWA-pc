#include "common.h"
#include "runtime/vfs.h"

namespace hle {

namespace {

constexpr int64_t CELL_GAME_ERROR_PARAM = static_cast<int32_t>(0x8002cb07);
constexpr int64_t CELL_GAME_ERROR_NOTPATCH = static_cast<int32_t>(0x8002cb0e);
constexpr int64_t CELL_GAME_RET_NONE = 2;

std::string param_string(uint32_t id) {
    const vfs::Psf& p = vfs::patch_sfo().strings.empty() ? vfs::disc_sfo() : vfs::patch_sfo();
    const vfs::Psf& d = vfs::disc_sfo();
    switch (id) {
    case 0:
    case 1:
    case 3: return d.str("TITLE");
    case 100: return d.str("TITLE_ID");
    case 101: return p.str("VERSION").empty() ? d.str("VERSION") : p.str("VERSION");
    case 102: return d.str("PS3_SYSTEM_VER");
    case 104: return p.str("APP_VER").empty() ? d.str("APP_VER") : p.str("APP_VER");
    default:
        if (id >= 2 && id <= 20) {
            char key[16];
            std::snprintf(key, sizeof key, "TITLE_%02u", id - 2);
            std::string v = d.str(key);
            return v.empty() ? d.str("TITLE") : v;
        }
        return {};
    }
}

}

HLE_FUNC(cellGame, cellGameBootCheck) {
    uint32_t type = a32(ctx, 0), attr = a32(ctx, 1), size = a32(ctx, 2);
    if (!type || !attr) return ret(ctx, CELL_GAME_ERROR_PARAM);
    vm::write32(type, 1);
    vm::write32(attr, 0);
    if (size) {
        vm::write32(size, 40 * 1024 * 1024 - 256);
        vm::write32(size + 4, 0xFFFFFFFF);
        vm::write32(size + 8, 4);
    }
    LOG_INFO("cellGame", "cellGameBootCheck -> DISC");
    ret(ctx, 0);
}

HLE_FUNC(cellGame, cellGamePatchCheck) {
    ret(ctx, CELL_GAME_ERROR_NOTPATCH);
}

HLE_FUNC(cellGame, cellGameDataCheck) {
    uint32_t type = a32(ctx, 0);
    std::string dir = gstr(a32(ctx, 1));
    uint32_t size = a32(ctx, 2);
    if (size) {
        vm::write32(size, 40 * 1024 * 1024 - 256);
        vm::write32(size + 4, 0xFFFFFFFF);
        vm::write32(size + 8, 4);
    }
    LOG_INFO("cellGame", "cellGameDataCheck(%u, '%s')", type, dir.c_str());
    if (type == 1) return ret(ctx, 0);
    ret(ctx, CELL_GAME_RET_NONE);
}

HLE_FUNC(cellGame, cellGameContentPermit) {
    uint32_t info = a32(ctx, 0), usrdir = a32(ctx, 1);
    if (!info || !usrdir) return ret(ctx, CELL_GAME_ERROR_PARAM);
    put_str(info, "/dev_bdvd/PS3_GAME", 128);
    put_str(usrdir, "/dev_bdvd/PS3_GAME/USRDIR", 128);
    ret(ctx, 0);
}

HLE_FUNC(cellGame, cellGameGetParamString) {
    uint32_t id = a32(ctx, 0), buf = a32(ctx, 1), size = a32(ctx, 2);
    std::string v = param_string(id);
    put_str(buf, v, size);
    LOG_INFO("cellGame", "cellGameGetParamString(%u) -> '%s'", id, v.c_str());
    ret(ctx, 0);
}

HLE_FUNC(cellGame, cellGameSetParamString) {
    ret(ctx, 0);
}

HLE_FUNC(cellGame, cellGameCreateGameData) {
    LOG_WARN("cellGame", "cellGameCreateGameData not supported");
    ret(ctx, 0);
}

HLE_FUNC(cellGame, cellGameContentErrorDialog) {
    LOG_WARN("cellGame", "cellGameContentErrorDialog(%d, %d)", s32(ctx, 0), s32(ctx, 1));
    ret(ctx, 0);
}

}
