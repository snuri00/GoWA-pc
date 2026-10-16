#include "event.h"
#include "../spu/thread.h"
#include "runtime/vfs.h"
#include "ppu/ops.h"

#include <array>
#include <cstdio>
#include <vector>

namespace lv2 {

namespace {

using Segment = std::array<uint32_t, 4>;

struct SpuImage : Object {
    uint32_t entry = 0;
    std::vector<Segment> segments;
    uint32_t mem = 0;
};

std::mutex g_lock;
std::map<uint32_t, std::shared_ptr<spu::Group>> g_groups;
uint32_t g_next_group = 0;

constexpr uint64_t SPU_GROUP_RUN_KEY = 0xFFFFFFFF53505500ull;

std::shared_ptr<spu::Group> find_group(uint32_t id) {
    std::lock_guard lk(g_lock);
    auto it = g_groups.find(id);
    return it == g_groups.end() ? nullptr : it->second;
}

std::shared_ptr<spu::SpuThread> find_thread(uint32_t id) {
    if (id >= 0x06000000) return nullptr;
    auto g = find_group((id & 0xFFFFFF) | 0x04000000);
    if (!g) return nullptr;
    uint32_t idx = id >> 24;
    std::lock_guard lk(g->m);
    if (idx >= g->threads.size()) return nullptr;
    return g->threads[idx];
}

uint32_t be32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return __builtin_bswap32(v); }
uint16_t be16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return __builtin_bswap16(v); }

int64_t load_image(const uint8_t* data, size_t size, uint32_t img) {
    if (size < 0x34 || std::memcmp(data, "\x7f" "ELF", 4) != 0 || data[4] != 1 || be16(data + 0x12) != 23) return CELL_ENOEXEC;
    uint32_t entry = be32(data + 0x18);
    uint32_t phoff = be32(data + 0x1C);
    uint16_t phes = be16(data + 0x2A);
    uint16_t phn = be16(data + 0x2C);
    uint32_t mem = vm::alloc(vm::Region::User64K, static_cast<uint32_t>(size), 0x10000);
    if (!mem) return CELL_ENOMEM;
    std::memcpy(vm::ptr(mem), data, size);
    auto im = std::make_shared<SpuImage>();
    im->entry = entry;
    im->mem = mem;
    for (uint16_t i = 0; i < phn; i++) {
        const uint8_t* ph = data + phoff + i * phes;
        uint32_t type = be32(ph), off = be32(ph + 4), vaddr = be32(ph + 8), filesz = be32(ph + 16), memsz = be32(ph + 20);
        if (type == 1) {
            if (filesz) im->segments.push_back({1, vaddr, filesz, mem + off});
            if (memsz > filesz) im->segments.push_back({2, vaddr + filesz, memsz - filesz, 0});
        } else if (type == 4) {
            im->segments.push_back({4, 0, 0x20, mem + off + 0x14});
        }
    }
    if (im->segments.empty() || im->segments.size() > 32) return CELL_ENOEXEC;
    uint32_t id = add_object(0x22000000, im);
    vm::write32(img, 1);
    vm::write32(img + 4, id);
    vm::write32(img + 8, 0);
    vm::write32(img + 12, 0);
    LOG_INFO("spu", "image 0x%x entry 0x%x segments %zu", id, entry, im->segments.size());
    return CELL_OK;
}

std::vector<uint8_t> read_host(const std::string& path) {
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

LV2_SYSCALL(156, sys_spu_image_open) {
    std::string path = reinterpret_cast<const char*>(vm::ptr(u32arg(ctx, 1)));
    auto data = read_host(vfs::host_path(path));
    LOG_INFO("spu", "sys_spu_image_open('%s') %zu bytes", path.c_str(), data.size());
    if (data.empty()) return CELL_ENOENT;
    return load_image(data.data(), data.size(), u32arg(ctx, 0));
}

LV2_SYSCALL(157, _sys_spu_image_import) {
    return load_image(vm::ptr(u32arg(ctx, 1)), u32arg(ctx, 2), u32arg(ctx, 0));
}

LV2_SYSCALL(155, _sys_spu_image_get_information) {
    uint32_t img = u32arg(ctx, 0);
    if (vm::read32(img) != 1) return CELL_EINVAL;
    auto im = get<SpuImage>(vm::read32(img + 4));
    if (!im) return CELL_ESRCH;
    vm::write32(u32arg(ctx, 1), im->entry);
    vm::write32(u32arg(ctx, 2), static_cast<uint32_t>(im->segments.size()));
    return CELL_OK;
}

LV2_SYSCALL(159, _sys_spu_image_get_segments) {
    uint32_t img = u32arg(ctx, 0), out = u32arg(ctx, 1);
    int32_t n = s32arg(ctx, 2);
    if (n < 1 || n > 32 || vm::read32(img) != 1) return CELL_EINVAL;
    auto im = get<SpuImage>(vm::read32(img + 4));
    if (!im) return CELL_ESRCH;
    for (int32_t i = 0; i < n && i < static_cast<int32_t>(im->segments.size()); i++) {
        const Segment& s = im->segments[i];
        uint32_t e = out + i * 0x18;
        vm::write32(e, s[0]);
        vm::write32(e + 4, s[1]);
        vm::write32(e + 8, s[2]);
        vm::write32(e + 12, 0);
        vm::write32(e + 16, s[3]);
        vm::write32(e + 20, 0);
    }
    return CELL_OK;
}

LV2_SYSCALL(158, _sys_spu_image_close) {
    uint32_t img = u32arg(ctx, 0);
    if (vm::read32(img) != 1) return CELL_EINVAL;
    uint32_t id = vm::read32(img + 4);
    auto im = get<SpuImage>(id);
    if (!im) return CELL_ESRCH;
    vm::dealloc(im->mem);
    remove_object(id);
    return CELL_OK;
}

LV2_SYSCALL(169, sys_spu_initialize) {
    if (u32arg(ctx, 1) > 5) return CELL_EINVAL;
    return CELL_OK;
}

LV2_SYSCALL(170, sys_spu_thread_group_create) {
    uint32_t out = u32arg(ctx, 0);
    uint32_t num = u32arg(ctx, 1);
    int32_t prio = s32arg(ctx, 2);
    uint32_t attr = u32arg(ctx, 3);
    if (!num || num > 6) return CELL_EINVAL;
    auto g = std::make_shared<spu::Group>();
    g->num = num;
    g->prio = prio;
    if (attr) {
        uint32_t nsize = vm::read32(attr);
        uint32_t np = vm::read32(attr + 4);
        g->type = static_cast<int32_t>(vm::read32(attr + 8));
        if (nsize > 0x80) return CELL_EINVAL;
        if (np && nsize) g->name.assign(reinterpret_cast<const char*>(vm::ptr(np)), strnlen(reinterpret_cast<const char*>(vm::ptr(np)), nsize));
    }
    g->threads.resize(num);
    {
        std::lock_guard lk(g_lock);
        g->id = 0x04000100 + (g_next_group++ % 255) * 0x100;
        g_groups[g->id] = g;
    }
    vm::write32(out, g->id);
    LOG_INFO("spu", "group_create '%s' num %u prio %d type 0x%x -> 0x%x", g->name.c_str(), num, prio, g->type, g->id);
    return CELL_OK;
}

LV2_SYSCALL(171, sys_spu_thread_group_destroy) {
    auto g = find_group(u32arg(ctx, 0));
    if (!g) return CELL_ESRCH;
    {
        std::lock_guard lk(g->m);
        if (g->state > 1) return CELL_EBUSY;
    }
    std::lock_guard lk(g_lock);
    g_groups.erase(g->id);
    return CELL_OK;
}

LV2_SYSCALL(172, sys_spu_thread_initialize) {
    uint32_t out = u32arg(ctx, 0);
    uint32_t gid = u32arg(ctx, 1);
    uint32_t slot = u32arg(ctx, 2);
    uint32_t img = u32arg(ctx, 3);
    uint32_t attr = u32arg(ctx, 4);
    uint32_t arg = u32arg(ctx, 5);
    if (slot >= 256) return CELL_EINVAL;
    if (!attr || !arg || !img) return CELL_EFAULT;
    auto g = find_group(gid);
    if (!g) return CELL_ESRCH;
    auto t = std::make_shared<spu::SpuThread>();
    uint32_t type = vm::read32(img);
    if (type == 1) {
        auto im = get<SpuImage>(vm::read32(img + 4));
        if (!im) return CELL_ESRCH;
        t->image_entry = im->entry;
        t->segments = im->segments;
    } else {
        t->image_entry = vm::read32(img + 4);
        uint32_t segs = vm::read32(img + 8);
        int32_t n = static_cast<int32_t>(vm::read32(img + 12));
        if (n < 1 || n > 32) return CELL_EINVAL;
        for (int32_t i = 0; i < n; i++) {
            uint32_t e = segs + i * 0x18;
            t->segments.push_back({vm::read32(e), vm::read32(e + 4), vm::read32(e + 8), vm::read32(e + 16)});
        }
    }
    uint32_t np = vm::read32(attr), nlen = vm::read32(attr + 4);
    if (nlen > 0x80) return CELL_EINVAL;
    if (np && nlen) t->name.assign(reinterpret_cast<const char*>(vm::ptr(np)), strnlen(reinterpret_cast<const char*>(vm::ptr(np)), nlen));
    t->option = vm::read32(attr + 8);
    for (int i = 0; i < 4; i++) t->args[i] = vm::read64(arg + i * 8);
    t->slot = slot;
    t->group = g.get();
    std::lock_guard lk(g->m);
    if (g->state != 0) return CELL_EBUSY;
    if (g->by_slot(slot)) return CELL_EBUSY;
    if (g->init >= g->num) return CELL_EBUSY;
    t->order = g->init;
    t->id = (g->init << 24) | (g->id & 0xFFFFFF);
    g->threads[g->init] = t;
    if (++g->init == g->num) g->state = 1;
    vm::write32(out, t->id);
    LOG_INFO("spu", "thread_initialize '%s' slot %u group 0x%x entry 0x%x -> 0x%x", t->name.c_str(), slot, gid, t->image_entry, t->id);
    return CELL_OK;
}

LV2_SYSCALL(173, sys_spu_thread_group_start) {
    auto g = find_group(u32arg(ctx, 0));
    if (!g) return CELL_ESRCH;
    uint32_t run_queue;
    {
        std::lock_guard lk(g->m);
        if (g->state != 1) return CELL_ESTAT;
        g->state = 6;
        g->join_state = 0;
        g->exit_status = 0;
        g->running = g->num;
        run_queue = g->run_queue;
    }
    LOG_INFO("spu", "group_start 0x%x '%s'", g->id, g->name.c_str());
    if (run_queue) {
        if (auto q = get<EventQueue>(run_queue)) push_event(q, {SPU_GROUP_RUN_KEY, g->id, 0, 0});
    }
    for (auto& t : g->threads) t->start();
    return CELL_OK;
}

LV2_SYSCALL(177, sys_spu_thread_group_terminate) {
    auto g = find_group(u32arg(ctx, 0));
    if (!g) return CELL_ESRCH;
    {
        std::lock_guard lk(g->m);
        if (g->state <= 1 || g->state == 3 || g->state == 5) return CELL_ESTAT;
        g->join_state = 4;
        g->exit_status = u32arg(ctx, 1);
    }
    g->stop_all();
    std::unique_lock lk(g->m);
    g->cv.wait(lk, [&] { return g->running == 0; });
    return CELL_OK;
}

LV2_SYSCALL(178, sys_spu_thread_group_join) {
    auto g = find_group(u32arg(ctx, 0));
    if (!g) return CELL_ESRCH;
    uint32_t cause = u32arg(ctx, 1), status = u32arg(ctx, 2);
    std::unique_lock lk(g->m);
    if (g->state == 0) return CELL_ESTAT;
    if (g->joining) return CELL_EBUSY;
    g->joining = true;
    g->cv.wait(lk, [&] { return g->state == 1 && g->join_state != 0; });
    g->joining = false;
    if (cause) vm::write32(cause, g->join_state);
    if (status) vm::write32(status, g->exit_status);
    g->join_state = 0;
    return cause ? CELL_OK : CELL_EFAULT;
}

LV2_SYSCALL(181, sys_spu_thread_write_ls) {
    auto t = find_thread(u32arg(ctx, 0));
    if (!t) return CELL_ESRCH;
    uint32_t lsa = u32arg(ctx, 1), type = u32arg(ctx, 3);
    uint64_t v = u64arg(ctx, 2);
    if (lsa >= 0x40000 || (type != 1 && type != 2 && type != 4 && type != 8) || lsa % type) return CELL_EINVAL;
    for (uint32_t i = 0; i < type; i++) t->ls[lsa + i] = static_cast<uint8_t>(v >> (8 * (type - 1 - i)));
    return CELL_OK;
}

LV2_SYSCALL(182, sys_spu_thread_read_ls) {
    auto t = find_thread(u32arg(ctx, 0));
    if (!t) return CELL_ESRCH;
    uint32_t lsa = u32arg(ctx, 1), out = u32arg(ctx, 2), type = u32arg(ctx, 3);
    if (lsa >= 0x40000 || (type != 1 && type != 2 && type != 4 && type != 8) || lsa % type) return CELL_EINVAL;
    uint64_t v = 0;
    for (uint32_t i = 0; i < type; i++) v = (v << 8) | t->ls[lsa + i];
    vm::write64(out, v);
    return CELL_OK;
}

LV2_SYSCALL(184, sys_spu_thread_write_snr) {
    auto t = find_thread(u32arg(ctx, 0));
    if (!t) return CELL_ESRCH;
    if (u32arg(ctx, 1) > 1) return CELL_EINVAL;
    t->push_snr(u32arg(ctx, 1), u32arg(ctx, 2));
    return CELL_OK;
}

LV2_SYSCALL(185, sys_spu_thread_group_connect_event) {
    auto g = find_group(u32arg(ctx, 0));
    if (!g) return CELL_ESRCH;
    uint32_t eq = u32arg(ctx, 1), et = u32arg(ctx, 2);
    std::lock_guard lk(g->m);
    if (et == 4 && !(g->type & 0x20)) return CELL_EINVAL;
    uint32_t* slot = et == 1 ? &g->run_queue : et == 2 ? &g->exception_queue : et == 4 ? &g->system_queue : nullptr;
    if (!slot) return CELL_EINVAL;
    if (*slot) return CELL_EBUSY;
    if (!get<EventQueue>(eq)) return CELL_ESRCH;
    *slot = eq;
    return CELL_OK;
}

LV2_SYSCALL(186, sys_spu_thread_group_disconnect_event) {
    auto g = find_group(u32arg(ctx, 0));
    if (!g) return CELL_ESRCH;
    std::lock_guard lk(g->m);
    uint32_t et = u32arg(ctx, 1);
    if (et == 1) g->run_queue = 0;
    if (et == 2) g->exception_queue = 0;
    if (et == 4) g->system_queue = 0;
    return CELL_OK;
}

LV2_SYSCALL(187, sys_spu_thread_set_spu_cfg) {
    auto t = find_thread(u32arg(ctx, 0));
    if (!t) return CELL_ESRCH;
    if (u64arg(ctx, 1) > 3) return CELL_EINVAL;
    t->snr_config = static_cast<uint32_t>(u64arg(ctx, 1));
    return CELL_OK;
}

LV2_SYSCALL(188, sys_spu_thread_get_spu_cfg) {
    auto t = find_thread(u32arg(ctx, 0));
    if (!t) return CELL_ESRCH;
    vm::write64(u32arg(ctx, 1), t->snr_config);
    return CELL_OK;
}

LV2_SYSCALL(190, sys_spu_thread_write_spu_mb) {
    auto t = find_thread(u32arg(ctx, 0));
    if (!t) return CELL_ESRCH;
    t->push_in_mbox(u32arg(ctx, 1));
    return CELL_OK;
}

LV2_SYSCALL(191, sys_spu_thread_connect_event) {
    auto t = find_thread(u32arg(ctx, 0));
    if (!t || !get<EventQueue>(u32arg(ctx, 1))) return CELL_ESRCH;
    uint32_t et = u32arg(ctx, 2), port = u32arg(ctx, 3);
    if (et != 1 || port > 63) return CELL_EINVAL;
    if (t->spup[port]) return CELL_EISCONN;
    t->spup[port] = u32arg(ctx, 1);
    return CELL_OK;
}

LV2_SYSCALL(192, sys_spu_thread_disconnect_event) {
    auto t = find_thread(u32arg(ctx, 0));
    if (!t) return CELL_ESRCH;
    uint32_t et = u32arg(ctx, 1), port = u32arg(ctx, 2);
    if (et != 1 || port > 63) return CELL_EINVAL;
    if (!t->spup[port]) return CELL_ENOTCONN;
    t->spup[port] = 0;
    return CELL_OK;
}

LV2_SYSCALL(193, sys_spu_thread_bind_queue) {
    auto t = find_thread(u32arg(ctx, 0));
    auto q = get<EventQueue>(u32arg(ctx, 1));
    if (!t || !q) return CELL_ESRCH;
    if (q->type != 2) return CELL_EINVAL;
    uint32_t num = u32arg(ctx, 2);
    for (auto& [n, qid] : t->spuq) {
        if (qid && (n == num || qid == u32arg(ctx, 1))) return CELL_EBUSY;
    }
    for (auto& [n, qid] : t->spuq) {
        if (!qid) {
            n = num;
            qid = u32arg(ctx, 1);
            return CELL_OK;
        }
    }
    return CELL_EAGAIN;
}

LV2_SYSCALL(194, sys_spu_thread_unbind_queue) {
    auto t = find_thread(u32arg(ctx, 0));
    if (!t) return CELL_ESRCH;
    for (auto& [n, qid] : t->spuq) {
        if (qid && n == u32arg(ctx, 1)) {
            qid = 0;
            return CELL_OK;
        }
    }
    return CELL_ESRCH;
}

LV2_SYSCALL(250, sys_spu_thread_group_set_cooperative_victims) {
    return CELL_OK;
}

LV2_SYSCALL(251, sys_spu_thread_group_connect_event_all_threads) {
    auto g = find_group(u32arg(ctx, 0));
    uint32_t eq = u32arg(ctx, 1);
    uint64_t req = u64arg(ctx, 2);
    uint32_t out = u32arg(ctx, 3);
    if (!req) return CELL_EINVAL;
    if (!g || !get<EventQueue>(eq)) return CELL_ESRCH;
    std::lock_guard lk(g->m);
    if (g->state == 0) return CELL_ESTAT;
    for (uint32_t p = 0; p < 64; p++) {
        if (!(req & (1ull << p))) continue;
        bool free = true;
        for (auto& t : g->threads) {
            if (t && t->spup[p]) free = false;
        }
        if (!free) continue;
        for (auto& t : g->threads) {
            if (t) t->spup[p] = eq;
        }
        vm::ptr(out)[0] = static_cast<uint8_t>(p);
        return CELL_OK;
    }
    return CELL_EISCONN;
}

LV2_SYSCALL(252, sys_spu_thread_group_disconnect_event_all_threads) {
    auto g = find_group(u32arg(ctx, 0));
    if (!g) return CELL_ESRCH;
    uint32_t p = u32arg(ctx, 1);
    if (p > 63) return CELL_EINVAL;
    std::lock_guard lk(g->m);
    for (auto& t : g->threads) {
        if (t) t->spup[p] = 0;
    }
    return CELL_OK;
}

LV2_SYSCALL(253, sys_spu_thread_group_syscall_253) {
    auto g = find_group(u32arg(ctx, 0));
    if (!g) return CELL_ESRCH;
    if (!(g->type & 0x20)) return CELL_EINVAL;
    uint32_t info = u32arg(ctx, 1);
    vm::write32(info, 0);
    vm::write32(info + 4, 0);
    vm::write64(info + 8, ppu::timebase());
    return CELL_OK;
}

LV2_SYSCALL(254, sys_spu_thread_group_log) {
    int32_t cmd = s32arg(ctx, 0);
    if (cmd == 2) {
        uint32_t st = u32arg(ctx, 1);
        if (!st) return CELL_EFAULT;
        vm::write32(st, 1);
        return CELL_OK;
    }
    return cmd == 0 || cmd == 1 ? CELL_OK : CELL_EINVAL;
}

}
