#include "common.h"

namespace hle {

namespace {

constexpr int32_t SCE_NP_MANAGER_STATUS_OFFLINE = -1;
constexpr int32_t CELL_NET_CTL_STATE_Disconnected = 0;
constexpr int64_t SCE_NP_ERROR_NOT_INITIALIZED = static_cast<int32_t>(0x8002aa01);
constexpr int64_t CELL_NET_CTL_ERROR_NOT_CONNECTED = static_cast<int32_t>(0x80130106);

}

HLE_FUNC(sys_net, sys_net_initialize_network_ex) { ret(ctx, 0); }
HLE_FUNC(sys_net, sys_net_finalize_network) { ret(ctx, 0); }
HLE_FUNC(cellNetCtl, cellNetCtlInit) { ret(ctx, 0); }
HLE_FUNC(cellNetCtl, cellNetCtlTerm) { ret(ctx, 0); }
HLE_FUNC(cellNetCtl, cellNetCtlAddHandler) {
    if (uint32_t id = a32(ctx, 2)) vm::write32(id, 1);
    ret(ctx, 0);
}
HLE_FUNC(cellNetCtl, cellNetCtlDelHandler) { ret(ctx, 0); }
HLE_FUNC(cellNetCtl, cellNetCtlGetState) {
    if (uint32_t p = a32(ctx, 0)) vm::write32(p, CELL_NET_CTL_STATE_Disconnected);
    ret(ctx, 0);
}
HLE_FUNC(cellNetCtl, cellNetCtlGetInfo) { ret(ctx, CELL_NET_CTL_ERROR_NOT_CONNECTED); }
HLE_FUNC(cellNetCtl, cellNetCtlGetNatInfo) { ret(ctx, CELL_NET_CTL_ERROR_NOT_CONNECTED); }

HLE_FUNC(sceNp2, sceNp2Init) { ret(ctx, 0); }
HLE_FUNC(sceNp2, sceNp2Term) { ret(ctx, 0); }
HLE_FUNC(sceNp, sceNpInit) { ret(ctx, 0); }
HLE_FUNC(sceNp, sceNpTerm) { ret(ctx, 0); }
HLE_FUNC(sceNp, sceNpLookupInit) { ret(ctx, 0); }
HLE_FUNC(sceNp, sceNpManagerRegisterCallback) { ret(ctx, 0); }
HLE_FUNC(sceNp, sceNpManagerUnregisterCallback) { ret(ctx, 0); }
HLE_FUNC(sceNp, sceNpBasicRegisterContextSensitiveHandler) { ret(ctx, 0); }
HLE_FUNC(sceNp, sceNpBasicRegisterHandler) { ret(ctx, 0); }
HLE_FUNC(sceNp, sceNpManagerGetStatus) {
    if (uint32_t p = a32(ctx, 0)) vm::write32(p, static_cast<uint32_t>(SCE_NP_MANAGER_STATUS_OFFLINE));
    ret(ctx, 0);
}
HLE_FUNC(sceNp, sceNpManagerGetNpId) { ret(ctx, static_cast<int32_t>(0x8002aa0c)); }
HLE_FUNC(sceNp, sceNpManagerGetOnlineId) { ret(ctx, static_cast<int32_t>(0x8002aa0c)); }
HLE_FUNC(sceNp, sceNpManagerGetAccountRegion) { ret(ctx, static_cast<int32_t>(0x8002aa0c)); }
HLE_FUNC(sceNp, sceNpManagerGetTicket) { ret(ctx, static_cast<int32_t>(0x8002aa0c)); }
HLE_FUNC(sceNp, sceNpManagerGetChatRestrictionFlag) {
    if (uint32_t p = a32(ctx, 0)) vm::write32(p, 1);
    ret(ctx, 0);
}
HLE_FUNC(sceNp, sceNpManagerGetContentRatingFlag) {
    if (uint32_t p = a32(ctx, 0)) vm::write32(p, 0);
    if (uint32_t p = a32(ctx, 1)) vm::write32(p, 0);
    ret(ctx, 0);
}

HLE_FUNC(sceNpTrophy, sceNpTrophyInit) { ret(ctx, 0); }
HLE_FUNC(sceNpTrophy, sceNpTrophyTerm) { ret(ctx, 0); }
HLE_FUNC(sceNpTrophy, sceNpTrophyCreateContext) {
    if (uint32_t p = a32(ctx, 0)) vm::write32(p, 1);
    ret(ctx, 0);
}
HLE_FUNC(sceNpTrophy, sceNpTrophyCreateHandle) {
    if (uint32_t p = a32(ctx, 0)) vm::write32(p, 1);
    ret(ctx, 0);
}
HLE_FUNC(sceNpTrophy, sceNpTrophyRegisterContext) {
    uint32_t cb = a32(ctx, 2), user = a32(ctx, 3);
    if (cb) {
        call_guest(ctx, cb, 1, 0, 0, user);
        call_guest(ctx, cb, 1, 6, 0, user);
    }
    ret(ctx, 0);
}
HLE_FUNC(sceNpTrophy, sceNpTrophyGetRequiredDiskSpace) {
    if (uint32_t p = a32(ctx, 2)) vm::write64(p, 0);
    ret(ctx, 0);
}
HLE_FUNC(sceNpTrophy, sceNpTrophyUnlockTrophy) {
    LOG_INFO("trophy", "unlock trophy %d", s32(ctx, 2));
    if (uint32_t p = a32(ctx, 3)) vm::write32(p, 0xFFFFFFFF);
    ret(ctx, 0);
}
HLE_FUNC(sceNpTrophy, sceNpTrophyGetTrophyUnlockState) {
    if (uint32_t p = a32(ctx, 2)) std::memset(vm::ptr(p), 0, 16);
    if (uint32_t p = a32(ctx, 3)) vm::write32(p, 0);
    ret(ctx, 0);
}
HLE_FUNC(sceNpTrophy, sceNpTrophyGetGameProgress) {
    if (uint32_t p = a32(ctx, 2)) vm::write32(p, 0);
    ret(ctx, 0);
}

}
