#include "common.h"
#include "../lv2/event.h"

#include <map>
#include <mutex>

namespace hle {

namespace {

constexpr int32_t CELL_VOICE_ERROR_LIBVOICE_NOT_INIT = static_cast<int32_t>(0x80310801);
constexpr int32_t CELL_VOICE_ERROR_LIBVOICE_INITIALIZED = static_cast<int32_t>(0x80310802);
constexpr int32_t CELL_VOICE_ERROR_ARGUMENT_INVALID = static_cast<int32_t>(0x80310805);
constexpr int32_t CELL_VOICE_ERROR_TOPOLOGY = static_cast<int32_t>(0x8031080a);

std::mutex g_lock;
bool g_init = false;
uint32_t g_next_port = 1;
std::map<uint32_t, uint32_t> g_ports;

}

HLE_FUNC(cellVoice, cellVoiceInit) {
    std::lock_guard lk(g_lock);
    if (g_init) return ret(ctx, CELL_VOICE_ERROR_LIBVOICE_INITIALIZED);
    if (!a32(ctx, 0)) return ret(ctx, CELL_VOICE_ERROR_ARGUMENT_INVALID);
    g_init = true;
    ret(ctx, 0);
}

HLE_FUNC(cellVoice, cellVoiceInitEx) {
    std::lock_guard lk(g_lock);
    if (g_init) return ret(ctx, CELL_VOICE_ERROR_LIBVOICE_INITIALIZED);
    g_init = true;
    ret(ctx, 0);
}

HLE_FUNC(cellVoice, cellVoiceEnd) {
    std::lock_guard lk(g_lock);
    g_init = false;
    g_ports.clear();
    ret(ctx, 0);
}

HLE_FUNC(cellVoice, cellVoiceCreateNotifyEventQueue) {
    uint32_t id_out = a32(ctx, 0), key_out = a32(ctx, 1);
    if (!g_init) return ret(ctx, CELL_VOICE_ERROR_LIBVOICE_NOT_INIT);
    for (uint64_t i = 0; i < 10; i++) {
        uint64_t key = 0x80004d494f323285ull + i;
        if (lv2::queue_by_key(key)) continue;
        auto q = std::make_shared<lv2::EventQueue>();
        q->key = key;
        q->size = 0x40;
        q->name = "cellVoice";
        vm::write32(id_out, lv2::add_object(0x8D000000, q));
        vm::write64(key_out, key);
        return ret(ctx, 0);
    }
    ret(ctx, static_cast<int32_t>(0x8031080b));
}

HLE_FUNC(cellVoice, cellVoiceSetNotifyEventQueue) {
    if (!g_init) return ret(ctx, CELL_VOICE_ERROR_LIBVOICE_NOT_INIT);
    ret(ctx, 0);
}

HLE_FUNC(cellVoice, cellVoiceRemoveNotifyEventQueue) {
    if (!g_init) return ret(ctx, CELL_VOICE_ERROR_LIBVOICE_NOT_INIT);
    ret(ctx, 0);
}

HLE_FUNC(cellVoice, cellVoiceCreatePort) {
    uint32_t out = a32(ctx, 0), arg = a32(ctx, 1);
    std::lock_guard lk(g_lock);
    if (!g_init) return ret(ctx, CELL_VOICE_ERROR_LIBVOICE_NOT_INIT);
    if (!out || !arg) return ret(ctx, CELL_VOICE_ERROR_ARGUMENT_INVALID);
    uint32_t id = g_next_port++;
    g_ports[id] = vm::read32(arg);
    vm::write32(out, id);
    ret(ctx, 0);
}

HLE_FUNC(cellVoice, cellVoiceDeletePort) {
    std::lock_guard lk(g_lock);
    if (!g_init) return ret(ctx, CELL_VOICE_ERROR_LIBVOICE_NOT_INIT);
    g_ports.erase(a32(ctx, 0));
    ret(ctx, 0);
}

HLE_FUNC(cellVoice, cellVoiceConnectIPortToOPort) {
    std::lock_guard lk(g_lock);
    if (!g_init) return ret(ctx, CELL_VOICE_ERROR_LIBVOICE_NOT_INIT);
    if (!g_ports.count(a32(ctx, 0)) || !g_ports.count(a32(ctx, 1))) return ret(ctx, CELL_VOICE_ERROR_TOPOLOGY);
    ret(ctx, 0);
}

HLE_FUNC(cellVoice, cellVoiceDisconnectIPortFromOPort) {
    if (!g_init) return ret(ctx, CELL_VOICE_ERROR_LIBVOICE_NOT_INIT);
    ret(ctx, 0);
}

HLE_FUNC(cellVoice, cellVoiceStart) {
    if (!g_init) return ret(ctx, CELL_VOICE_ERROR_LIBVOICE_NOT_INIT);
    ret(ctx, 0);
}

HLE_FUNC(cellVoice, cellVoiceStop) {
    if (!g_init) return ret(ctx, CELL_VOICE_ERROR_LIBVOICE_NOT_INIT);
    ret(ctx, 0);
}

HLE_FUNC(cellVoice, cellVoiceGetPortInfo) {
    uint32_t id = a32(ctx, 0), info = a32(ctx, 1);
    std::lock_guard lk(g_lock);
    if (!g_init) return ret(ctx, CELL_VOICE_ERROR_LIBVOICE_NOT_INIT);
    auto it = g_ports.find(id);
    if (it == g_ports.end()) return ret(ctx, CELL_VOICE_ERROR_TOPOLOGY);
    std::memset(vm::ptr(info), 0, 0x20);
    vm::write32(info, it->second);
    vm::write32(info + 4, 1);
    ret(ctx, 0);
}

HLE_FUNC(cellVoice, cellVoiceReadFromIPort) {
    if (uint32_t size = a32(ctx, 2)) vm::write32(size, 0);
    ret(ctx, 0);
}

HLE_FUNC(cellVoice, cellVoiceWriteToOPort) { ret(ctx, 0); }
HLE_FUNC(cellVoice, cellVoiceSetPortAttr) { ret(ctx, 0); }
HLE_FUNC(cellVoice, cellVoiceGetPortAttr) { ret(ctx, 0); }
HLE_FUNC(cellVoice, cellVoiceResetPort) { ret(ctx, 0); }
HLE_FUNC(cellVoice, cellVoicePausePort) { ret(ctx, 0); }
HLE_FUNC(cellVoice, cellVoiceResumePort) { ret(ctx, 0); }
HLE_FUNC(cellVoice, cellVoiceUpdatePort) { ret(ctx, 0); }
HLE_FUNC(cellVoice, cellVoiceGetMuteFlag) {
    if (uint32_t p = a32(ctx, 1)) vm::write16(p, 0);
    ret(ctx, 0);
}
HLE_FUNC(cellVoice, cellVoiceSetMuteFlag) { ret(ctx, 0); }
HLE_FUNC(cellVoice, cellVoiceSetVolume) { ret(ctx, 0); }
HLE_FUNC(cellVoice, cellVoiceGetVolume) { ret(ctx, 0); }
HLE_FUNC(cellVoice, cellVoiceSetBitRate) { ret(ctx, 0); }
HLE_FUNC(cellVoice, cellVoiceGetBitRate) { ret(ctx, 0); }

}

HLE_FUNC(cellLibprof, cellUserTraceRegister) { hle::ret(ctx, 0); }
HLE_FUNC(cellLibprof, cellUserTraceUnregister) { hle::ret(ctx, 0); }
