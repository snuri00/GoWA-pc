#include "thread.h"
#include "../lv2/event.h"
#include "../lv2/sync.h"
#include "ppu/ops.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/reservation.h"

#include <chrono>
#include <cstring>
#include <malloc.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace spu {

namespace {

enum : uint32_t {
    EV_LR = 0x400, EV_S1 = 0x200, EV_S2 = 0x100, EV_TM = 0x20, EV_SN = 0x2,
};

constexpr uint64_t SPU_USER_EVENT_KEY = 0xFFFFFFFF53505501ull;
constexpr uint64_t SPU_GROUP_RUN_KEY = 0xFFFFFFFF53505500ull;

uint64_t timebase_now() { return ppu::timebase(); }

}

SpuThread::SpuThread() {
    ls = static_cast<uint8_t*>(_aligned_malloc(LS_SIZE + 16, 128));
    std::memset(ls, 0, LS_SIZE + 16);
    ctx.ls = ls;
}

SpuThread::~SpuThread() { _aligned_free(ls); }

template <class Pred>
bool SpuThread::block(std::unique_lock<std::mutex>& lk, Pred pred) {
    while (!pred()) {
        if (stop_flag.load(std::memory_order_relaxed)) {
            ctx.halted = true;
            return false;
        }
        cv.wait_for(lk, std::chrono::microseconds(100));
    }
    return true;
}

uint32_t SpuThread::dec_now() {
    if (dec_frozen) return dec_value;
    return static_cast<uint32_t>(dec_value - (timebase_now() - dec_start));
}

void SpuThread::set_events(uint32_t bits) {
    events |= bits;
    if (event_mask & bits) event_count = true;
}

void SpuThread::collect_events() {
    if (!dec_frozen) {
        uint64_t elapsed = timebase_now() - dec_start;
        if (elapsed > dec_value) {
            set_events(EV_TM);
            dec_start += 0x100000000ull;
        }
    }
    if ((event_mask & EV_LR) && raddr && !(events & EV_LR)) {
        if (rsv::lost(raddr, rtime, rdata)) {
            set_events(EV_LR);
            raddr = 0;
        }
    }
    if (events & event_mask) event_count = true;
}

void SpuThread::push_snr(uint32_t number, uint32_t value) {
    std::lock_guard lk(m);
    bool orm = (snr_config >> number) & 1;
    bool was = snr_count[number];
    snr[number] = orm && was ? (snr[number] | value) : value;
    snr_count[number] = true;
    if (!was || !orm) set_events(number == 0 ? EV_S1 : EV_S2);
    cv.notify_all();
}

void SpuThread::push_in_mbox(uint32_t value) {
    std::lock_guard lk(m);
    if (in_mbox.size() >= 4) in_mbox.back() = value;
    else in_mbox.push_back(value);
    cv.notify_all();
}

uint32_t SpuThread::pop_out_mbox() {
    std::lock_guard lk(m);
    out_mbox_full = false;
    cv.notify_all();
    return out_mbox;
}

uint32_t SpuThread::channel_count(uint32_t ch) {
    std::lock_guard lk(m);
    switch (ch) {
    case 0: collect_events(); return event_count ? 1 : 0;
    case 3: return snr_count[0];
    case 4: return snr_count[1];
    case 21: return 16;
    case 23: return 1;
    case 24: return tag_stat_count;
    case 25: return stall_stat_count;
    case 27: return atomic_stat_count;
    case 28: return out_mbox_full ? 0 : 1;
    case 29: return static_cast<uint32_t>(in_mbox.size());
    case 30: return out_intr_full ? 0 : 1;
    case 9: return 0;
    default: return 1;
    }
}

uint32_t SpuThread::read_channel(uint32_t ch) {
    std::unique_lock lk(m);
    switch (ch) {
    case 0: {
        collect_events();
        while (!event_count) {
            if (stop_flag.load(std::memory_order_relaxed)) {
                ctx.halted = true;
                return 0;
            }
            if ((event_mask & EV_LR) && raddr) {
                uint32_t line = raddr;
                uint64_t rt = rtime;
                lk.unlock();
                rsv::wait_change(line, rt, 100);
                lk.lock();
            } else {
                cv.wait_for(lk, std::chrono::microseconds(100));
            }
            collect_events();
        }
        event_count = false;
        return events & event_mask;
    }
    case 3:
    case 4: {
        int n = ch - 3;
        if (!block(lk, [&] { return snr_count[n]; })) return 0;
        snr_count[n] = false;
        uint32_t v = snr[n];
        snr[n] = 0;
        return v;
    }
    case 8: return dec_now();
    case 11: return event_mask;
    case 12: return tag_mask;
    case 13: return ctx.interrupts_enabled ? 1 : 0;
    case 15: return ctx.srr0;
    case 24:
        if (!block(lk, [&] { return tag_stat_count; })) return 0;
        tag_stat_count = false;
        return tag_stat;
    case 25: {
        if (!block(lk, [&] { return stall_stat_count; })) return 0;
        stall_stat_count = false;
        uint32_t v = stall_stat;
        stall_stat = 0;
        return v;
    }
    case 27:
        if (!block(lk, [&] { return atomic_stat_count; })) return 0;
        atomic_stat_count = false;
        return atomic_stat;
    case 29: {
        if (!block(lk, [&] { return !in_mbox.empty(); })) return 0;
        uint32_t v = in_mbox.front();
        in_mbox.pop_front();
        return v;
    }
    default:
        LOG_ERROR("spu", "thread 0x%x read of unknown channel %u at 0x%05x", id, ch, ctx.pc);
        return 0;
    }
}

void SpuThread::write_channel(uint32_t ch, uint32_t value) {
    std::unique_lock lk(m);
    switch (ch) {
    case 1:
        collect_events();
        event_mask = value;
        if (events & event_mask) event_count = true;
        return;
    case 2:
        collect_events();
        if ((value & EV_TM) && !(event_mask & EV_TM)) {
            dec_value = dec_now();
            dec_frozen = true;
        }
        events &= ~value;
        event_count = (events & event_mask) != 0;
        return;
    case 7:
        collect_events();
        dec_start = timebase_now();
        dec_value = value;
        dec_frozen = false;
        return;
    case 14: ctx.srr0 = value & 0x3FFFC; return;
    case 16: mfc_lsa = value; return;
    case 17: return;
    case 18: mfc_eal = value; return;
    case 19: mfc_size = value > 0xFFFF ? 0xFFFF : value; return;
    case 20: mfc_tag = value & 0x1F; return;
    case 21:
        lk.unlock();
        process_mfc(value & 0xFF);
        return;
    case 22: tag_mask = value; return;
    case 23:
        tag_stat = tag_mask;
        if (value == 1 && tag_mask == 0) return;
        tag_stat_count = true;
        return;
    case 26: {
        uint32_t tag = value & 0x1F;
        auto st = stalled[tag];
        if (!st.active) return;
        stalled[tag].active = false;
        lk.unlock();
        run_list(tag, st.cmd, st.lsa, st.list, st.remaining);
        return;
    }
    case 28:
        if (!block(lk, [&] { return !out_mbox_full; })) return;
        out_mbox = value;
        out_mbox_full = true;
        return;
    case 30: {
        uint32_t code = value >> 24;
        uint32_t data0 = value & 0xFFFFFF;
        if (!out_mbox_full) {
            LOG_ERROR("spu", "thread 0x%x WrOutIntrMbox 0x%08x with empty out mbox", id, value);
            return;
        }
        uint32_t data1 = out_mbox;
        out_mbox_full = false;
        lk.unlock();
        if (code < 128) {
            uint32_t port = code & 63;
            bool reply = code < 64;
            if (reply) {
                std::lock_guard l2(m);
                if (!in_mbox.empty()) {
                    in_mbox.assign(1, static_cast<uint32_t>(lv2::CELL_EBUSY));
                    cv.notify_all();
                    return;
                }
            }
            int64_t r = lv2::CELL_ENOTCONN;
            if (uint32_t q = spup[port]) {
                if (auto queue = lv2::get<lv2::EventQueue>(q)) {
                    r = lv2::push_event(queue, {SPU_USER_EVENT_KEY, id, (uint64_t(port) << 32) | data0, data1});
                }
            }
            if (reply) push_in_mbox(static_cast<uint32_t>(r));
            else if (r) LOG_WARN("spu", "thread 0x%x throw_event port %u failed 0x%llx", id, port, static_cast<unsigned long long>(r));
        } else if (code == 128 || code == 192) {
            int64_t r = lv2::event_flag_set_bits(data1, 1ull << (data0 & 63));
            if (code == 128) push_in_mbox(static_cast<uint32_t>(r));
        } else {
            LOG_ERROR("spu", "thread 0x%x unknown WrOutIntrMbox code %u", id, code);
        }
        return;
    }
    case 9:
    case 69:
    case 70:
    case 71:
        return;
    default:
        LOG_ERROR("spu", "thread 0x%x write 0x%x to unknown channel %u at 0x%05x", id, value, ch, ctx.pc);
        return;
    }
}

uint8_t* SpuThread::translate(uint32_t ea, uint32_t size, bool write, bool& handled, uint32_t lsa) {
    handled = false;
    if (ea >= 0xF0000000 && group) {
        uint32_t slot_n = (ea - 0xF0000000) / 0x100000;
        uint32_t off = (ea - 0xF0000000) % 0x100000;
        SpuThread* t = group->by_slot(slot_n);
        if (t) {
            if (off + size <= LS_SIZE) return t->ls + off;
            if (write && size == 4 && (off == 0x5400C || off == 0x5C00C)) {
                uint32_t v;
                std::memcpy(&v, ls + (lsa & 0x3FFFF), 4);
                t->push_snr(off == 0x5400C ? 0 : 1, __builtin_bswap32(v));
                handled = true;
                return nullptr;
            }
        }
        LOG_ERROR("spu", "thread 0x%x DMA to unsupported SPU window 0x%08x size 0x%x", id, ea, size);
        handled = true;
        return nullptr;
    }
    return vm::ptr(ea);
}

void SpuThread::dma(uint32_t cmd, uint32_t lsa, uint32_t ea, uint32_t size) {
    if (!size) return;
    if (size > 0x4000) {
        LOG_ERROR("spu", "thread 0x%x DMA size 0x%x too large", id, size);
        return;
    }
    lsa &= 0x3FFFF;
    bool put = (cmd & 0xF0) == 0x20 || (cmd & 0xF0) == 0x30 || (cmd & 0xF0) == 0xA0;
    bool handled;
    uint8_t* p = translate(ea, size, put, handled, lsa);
    if (handled || !p) return;
    if (cmd == 0x89) {
        std::memset(p, 0, size);
        return;
    }
    if (put) std::memcpy(p, ls + lsa, size);
    else std::memcpy(ls + lsa, p, size);
}

bool SpuThread::run_list(uint32_t tag, uint32_t cmd, uint32_t lsa, uint32_t list, uint32_t size) {
    uint32_t base_cmd = cmd & ~0xFu;
    uint32_t cur = lsa & 0x3FFF0;
    list &= 0x3FFF8;
    while (size >= 8) {
        uint8_t* e = ls + list;
        bool stall = (e[0] & 0x80) != 0;
        uint32_t tsize = ((uint32_t(e[2]) << 8) | e[3]) & 0x7FFF;
        uint32_t ea;
        std::memcpy(&ea, e + 4, 4);
        ea = __builtin_bswap32(ea);
        dma(base_cmd, cur | (ea & 0xF), ea, tsize);
        cur = (cur + ((tsize + 15) & ~15u)) & 0x3FFF0;
        list = (list + 8) & 0x3FFF8;
        size -= 8;
        if (stall && size >= 8) {
            std::lock_guard lk(m);
            stalled[tag] = {true, cmd, cur, list, size};
            if (!stall_stat) set_events(EV_SN);
            stall_stat |= 1u << tag;
            stall_stat_count = true;
            cv.notify_all();
            return false;
        }
    }
    return true;
}

void SpuThread::process_mfc(uint32_t cmd) {
    uint32_t lsa = mfc_lsa, ea = mfc_eal, size = mfc_size, tag = mfc_tag;
    switch (cmd) {
    case 0x20: case 0x21: case 0x22: case 0x30: case 0x31: case 0x32:
    case 0x40: case 0x41: case 0x42: case 0x89:
        dma(cmd, lsa, ea, size);
        break;
    case 0xA0: case 0xA1: case 0xA2:
        dma(0x20, lsa, ea, 4);
        break;
    case 0x24: case 0x25: case 0x26: case 0x34: case 0x35: case 0x36:
    case 0x44: case 0x45: case 0x46:
        run_list(tag, cmd, lsa, ea, size);
        break;
    case 0xD0: {
        uint32_t line = ea & ~127u;
        std::lock_guard lk(m);
        if (raddr && raddr != line && rsv::lost(raddr, rtime, rdata)) set_events(EV_LR);
        uint8_t snap[128];
        uint64_t ver = rsv::snapshot(line, snap);
        if (raddr == line && ver != rtime && std::memcmp(snap, rdata, 128) == 0) set_events(EV_LR);
        raddr = line;
        rtime = ver;
        std::memcpy(rdata, snap, 128);
        std::memcpy(ls + (lsa & 0x3FF80), snap, 128);
        atomic_stat = 4;
        atomic_stat_count = true;
        return;
    }
    case 0xB4: {
        uint32_t line = ea & ~127u;
        std::lock_guard lk(m);
        bool ok = raddr == line && rsv::commit_if(line, rtime, rdata, ls + (lsa & 0x3FF80), 0, 128);
        if (!ok) set_events(EV_LR);
        raddr = 0;
        atomic_stat = ok ? 0 : 1;
        atomic_stat_count = true;
        return;
    }
    case 0xB0:
    case 0xB8: {
        uint32_t line = ea & ~127u;
        rsv::store_unconditional(line, ls + (lsa & 0x3FF80));
        if (cmd == 0xB0) {
            std::lock_guard lk(m);
            atomic_stat = 2;
            atomic_stat_count = true;
        }
        return;
    }
    case 0xC0: case 0xC8: case 0xCC: case 0x80: case 0x81:
        std::atomic_thread_fence(std::memory_order_seq_cst);
        break;
    default:
        LOG_ERROR("spu", "thread 0x%x unknown MFC command 0x%x", id, cmd);
        break;
    }
}

bool SpuThread::stop_and_signal(uint32_t code) {
    switch (code) {
    case 0x001:
        Sleep(1);
        return true;
    case 0x100:
        return true;
    case 0x101: {
        uint32_t status = pop_out_mbox();
        LOG_INFO("spu", "thread 0x%x group_exit(%u)", id, status);
        if (group) {
            std::lock_guard lk(group->m);
            if (!group->join_state) {
                group->join_state = 1;
                group->exit_status = status;
            }
        }
        if (group) group->stop_all();
        return false;
    }
    case 0x102: {
        exit_status = pop_out_mbox();
        exited_by_thread_exit = true;
        return false;
    }
    case 0x110:
    case 0x111: {
        uint32_t spuq_num;
        {
            std::lock_guard lk(m);
            spuq_num = out_mbox;
            out_mbox_full = false;
            if (!in_mbox.empty()) {
                in_mbox.assign(1, static_cast<uint32_t>(lv2::CELL_EBUSY));
                cv.notify_all();
                return true;
            }
        }
        uint32_t qid = 0;
        for (auto& [num, q] : spuq) {
            if (q && num == spuq_num) qid = q;
        }
        auto queue = qid ? lv2::get<lv2::EventQueue>(qid) : nullptr;
        if (!queue) {
            push_in_mbox(static_cast<uint32_t>(lv2::CELL_EINVAL));
            return true;
        }
        std::unique_lock ql(queue->m);
        while (queue->events.empty() && !queue->destroyed) {
            if (code == 0x111) {
                ql.unlock();
                push_in_mbox(static_cast<uint32_t>(lv2::CELL_EBUSY));
                return true;
            }
            if (stop_flag.load()) {
                ctx.halted = true;
                return false;
            }
            queue->cv.wait_for(ql, std::chrono::microseconds(500));
        }
        if (queue->destroyed) {
            ql.unlock();
            push_in_mbox(static_cast<uint32_t>(lv2::CELL_ECANCELED));
            return true;
        }
        lv2::Event e = queue->events.front();
        queue->events.pop_front();
        ql.unlock();
        std::lock_guard lk(m);
        in_mbox.assign({0u, static_cast<uint32_t>(e.data1), static_cast<uint32_t>(e.data2), static_cast<uint32_t>(e.data3)});
        cv.notify_all();
        return true;
    }
    default:
        LOG_ERROR("spu", "thread 0x%x unknown stop code 0x%x at 0x%05x", id, code, ctx.pc - 4);
        return false;
    }
}

void SpuThread::halt(uint32_t pc, uint32_t word) {
    LOG_ERROR("spu", "thread 0x%x '%s' halted at 0x%05x (0x%08x)", id, name.c_str(), pc, word);
}

void SpuThread::deploy() {
    for (auto& s : segments) {
        uint32_t type = s[0], lsa = s[1], size = s[2], val = s[3];
        if (type == 1) std::memcpy(ls + lsa, vm::ptr(val), size);
        else if (type == 2) {
            uint32_t be = __builtin_bswap32(val);
            for (uint32_t i = 0; i + 4 <= size; i += 4) std::memcpy(ls + lsa + i, &be, 4);
        }
    }
}

void SpuThread::start() {
    deploy();
    ctx = Context{};
    ctx.ls = ls;
    ctx.gpr[1].u32[3] = 0x3FFF0;
    for (int i = 0; i < 4; i++) ctx.gpr[3 + i].u64[1] = args[i];
    ctx.pc = image_entry & 0x3FFFC;
    ctx.interrupts_enabled = image_entry & 1;
    tag_mask = tag_stat = stall_stat = atomic_stat = 0;
    tag_stat_count = stall_stat_count = atomic_stat_count = false;
    out_mbox_full = out_intr_full = false;
    events = event_mask = 0;
    event_count = false;
    dec_value = (option & 2) ? ~static_cast<uint32_t>(timebase_now()) : 0;
    dec_start = timebase_now();
    dec_frozen = false;
    raddr = 0;
    exited_by_thread_exit = false;
    stop_flag = false;
    running = true;
    std::thread([this] { run(); }).detach();
}

void SpuThread::run() {
    wchar_t w[64];
    swprintf(w, 64, L"SPU %u %hs", slot, name.c_str());
    SetThreadDescription(GetCurrentThread(), w);
    _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x6000u | 0x8040u);
    LOG_INFO("spu", "thread 0x%x '%s' start pc 0x%05x args 0x%llx 0x%llx", id, name.c_str(), ctx.pc,
             static_cast<unsigned long long>(args[0]), static_cast<unsigned long long>(args[1]));
    uint32_t n = 0;
    while (step(ctx, *this)) {
        if ((++n & 1023) == 0 && stop_flag.load(std::memory_order_relaxed)) break;
    }
    LOG_INFO("spu", "thread 0x%x '%s' stopped at 0x%05x", id, name.c_str(), ctx.pc);
    {
        std::lock_guard lk(m);
        running = false;
        in_mbox.clear();
    }
    if (group) group->thread_stopped(*this);
}

SpuThread* Group::by_slot(uint32_t slot) {
    for (auto& t : threads) {
        if (t && t->slot == slot) return t.get();
    }
    return nullptr;
}

void Group::thread_stopped(SpuThread& t) {
    std::lock_guard lk(m);
    if (running) running--;
    if (!running) {
        state = 1;
        if (!join_state) {
            join_state = 2;
            exit_status = 0;
        }
        cv.notify_all();
        LOG_INFO("spu", "group 0x%x '%s' stopped (cause %u status %u)", id, name.c_str(), join_state, exit_status);
    }
}

void Group::stop_all() {
    for (auto& t : threads) {
        if (!t) continue;
        t->stop_flag = true;
        t->cv.notify_all();
    }
}

}
