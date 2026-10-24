#include "common.h"
#include "runtime/thread.h"
#include "runtime/video_decoder.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace hle {

namespace {

constexpr int32_t CELL_OK = 0;
constexpr int32_t VDEC_ERROR_ARG = static_cast<int32_t>(0x80610101);
constexpr int32_t VDEC_ERROR_SEQ = static_cast<int32_t>(0x80610102);
constexpr int32_t VDEC_ERROR_BUSY = static_cast<int32_t>(0x80610103);
constexpr int32_t VDEC_ERROR_EMPTY = static_cast<int32_t>(0x80610104);
constexpr int32_t VDEC_ERROR_FATAL = static_cast<int32_t>(0x80610180);

enum : uint32_t { MSG_AUDONE = 0, MSG_PICOUT = 1, MSG_SEQDONE = 2, MSG_ERROR = 3 };
enum : uint32_t { CODEC_MPEG2 = 0, CODEC_AVC = 1 };
enum : uint32_t { FMT_ARGB = 0, FMT_RGBA = 1, FMT_UYVY = 2, FMT_YUV420 = 3 };

constexpr uint32_t kItemSize = 0x50;
constexpr uint32_t kInfoSize = 0x160;
constexpr uint32_t kSlotSize = 0x200;
constexpr uint32_t kSlots = 32;
constexpr uint32_t kMemSize = 0x80000;
constexpr uint32_t kCmdDepth = 4;
constexpr size_t kMaxInflight = 16;
constexpr size_t kOutCap = 8;

bool env_on(const char* name, bool def) {
    const char* e = std::getenv(name);
    if (!e || !*e) return def;
    return e[0] != '0';
}

bool verbose() {
    static const bool v = env_on("GOWA_VDEC_LOG", false);
    return v;
}

bool hle_enabled() {
    static const bool on = [] {
        if (!env_on("GOWA_VDEC_HLE", true)) return false;
        bool ok = false;
        std::thread([&] { ok = media::create_decoder(media::Codec::Mpeg2) != nullptr; }).join();
        if (!ok) LOG_WARN("vdec", "no native MPEG-2 decoder (install the Microsoft MPEG-2 Video Extension); using LLE libvdec");
        else LOG_INFO("vdec", "native cellVdec enabled (GOWA_VDEC_HLE=0 restores LLE libvdec)");
        return ok;
    }();
    return on;
}

struct BitReader {
    const uint8_t* p;
    size_t n;
    size_t bit = 0;
    uint32_t get(int k) {
        uint32_t v = 0;
        for (int i = 0; i < k; i++, bit++) {
            size_t byte = bit >> 3;
            uint32_t b = byte < n ? (p[byte] >> (7 - (bit & 7))) & 1 : 0;
            v = (v << 1) | b;
        }
        return v;
    }
};

struct Mpeg2State {
    uint16_t hsize = 0, vsize = 0;
    uint8_t aspect = 0, frc = 0, progressive_seq = 0, low_delay = 0;
    uint8_t video_format = 5, colour_desc = 0, primaries = 0, transfer = 0, matrix = 0;
    uint16_t disp_h = 0, disp_v = 0;
    uint32_t time_code = 0;
    uint8_t closed_gop = 0, broken_link = 0;
    uint8_t mpeg1 = 1;
    uint32_t retained = 0;
};

struct PicHeader {
    uint16_t tref = 0;
    uint8_t type = 0;
    uint16_t vbv = 0;
    uint8_t structure = 3, tff = 0, rff = 0, progressive_frame = 1;
    uint8_t centre_n = 0;
    uint16_t fch[3] = {}, fcv[3] = {};
};

struct AuInfo {
    uint32_t start = 0, size = 0;
    uint32_t pts_hi = 0xFFFFFFFFu, pts_lo = 0xFFFFFFFFu, dts_hi = 0xFFFFFFFFu, dts_lo = 0xFFFFFFFFu;
    uint64_t user = 0;
};

struct Meta {
    AuInfo au[2];
    uint8_t au_num = 1;
    std::vector<uint8_t> info;
};

struct Picture {
    media::Frame frame;
    Meta meta;
    uint32_t conv_key = 0xFFFFFFFFu;
    std::vector<uint8_t> conv;
    bool item_given = false;
};

struct Cmd {
    enum Kind { Au, EndSeq, StartSeq } kind = Au;
    uint32_t mode = 0;
    AuInfo au;
    std::vector<uint8_t> data;
};

inline void put16(std::vector<uint8_t>& b, size_t o, uint16_t v) { b[o] = uint8_t(v >> 8); b[o + 1] = uint8_t(v); }
inline void put32(std::vector<uint8_t>& b, size_t o, uint32_t v) { for (int i = 0; i < 4; i++) b[o + i] = uint8_t(v >> (24 - 8 * i)); }

struct Decoder : std::enable_shared_from_this<Decoder> {
    uint32_t handle = 0;
    uint32_t codec = 0;
    uint32_t mem_addr = 0, mem_size = 0;
    uint32_t cb_func = 0, cb_arg = 0;
    std::mutex m;
    std::condition_variable cv;
    std::deque<Cmd> cmds;
    size_t inflight = 0;
    std::deque<Picture> out;
    bool closing = false;
    bool started = false;
    uint32_t slot = 0;
    uint32_t last_fmt_key = FMT_ARGB | (0xFFu << 8);
    std::shared_ptr<guest::Thread> thread;

    Mpeg2State seq;
    std::map<int64_t, Meta> metas;
    int64_t next_time = 0;
    int64_t pending_field = -1;
    uint64_t aus = 0, pics = 0;

    void callback(guest::Thread& t, uint32_t type, int32_t data) {
        if (!cb_func) return;
        call_guest(t.ctx, cb_func, handle, type, static_cast<uint32_t>(data), cb_arg);
    }

    std::vector<uint8_t> parse(const uint8_t* p, size_t n, PicHeader& ph, bool& seq_end, uint32_t& present) {
        present = 0;
        seq_end = false;
        int pic_index = -1;
        PicHeader cur;
        for (size_t i = 0; i + 4 <= n; i++) {
            if (p[i] || p[i + 1] || p[i + 2] != 1) continue;
            uint8_t code = p[i + 3];
            BitReader br{p + i + 4, n - i - 4};
            if (code == 0xB3) {
                seq.hsize = uint16_t(br.get(12));
                seq.vsize = uint16_t(br.get(12));
                seq.aspect = uint8_t(br.get(4));
                seq.frc = uint8_t(br.get(4));
                seq.mpeg1 = 1;
                present |= 0x1;
            } else if (code == 0xB5) {
                uint32_t id = br.get(4);
                if (id == 1) {
                    br.get(8);
                    seq.progressive_seq = uint8_t(br.get(1));
                    br.get(2);
                    seq.hsize = uint16_t((seq.hsize & 0xFFF) | (br.get(2) << 12));
                    seq.vsize = uint16_t((seq.vsize & 0xFFF) | (br.get(2) << 12));
                    br.get(12);
                    br.get(1);
                    br.get(8);
                    seq.low_delay = uint8_t(br.get(1));
                    seq.mpeg1 = 0;
                    present |= 0x2;
                } else if (id == 2) {
                    seq.video_format = uint8_t(br.get(3));
                    seq.colour_desc = uint8_t(br.get(1));
                    if (seq.colour_desc) {
                        seq.primaries = uint8_t(br.get(8));
                        seq.transfer = uint8_t(br.get(8));
                        seq.matrix = uint8_t(br.get(8));
                    }
                    seq.disp_h = uint16_t(br.get(14));
                    br.get(1);
                    seq.disp_v = uint16_t(br.get(14));
                    present |= 0x4;
                } else if (id == 8 && pic_index >= 0) {
                    br.get(16);
                    br.get(2);
                    cur.structure = uint8_t(br.get(2));
                    cur.tff = uint8_t(br.get(1));
                    br.get(5);
                    cur.rff = uint8_t(br.get(1));
                    br.get(1);
                    cur.progressive_frame = uint8_t(br.get(1));
                    present |= pic_index == 0 ? 0x200 : 0x2000;
                } else if (id == 7 && pic_index >= 0) {
                    int k = seq.progressive_seq ? (cur.rff ? (cur.tff ? 3 : 2) : 1) : (cur.structure != 3 ? 1 : (cur.rff ? 3 : 2));
                    cur.centre_n = uint8_t(k);
                    for (int j = 0; j < k; j++) {
                        cur.fch[j] = uint16_t(br.get(16));
                        br.get(1);
                        cur.fcv[j] = uint16_t(br.get(16));
                        br.get(1);
                    }
                    present |= pic_index == 0 ? 0x400 : 0x4000;
                }
            } else if (code == 0xB2) {
                present |= pic_index >= 0 ? (pic_index == 0 ? 0x800 : 0x8000) : (present & 0x20) ? 0x40 : 0x8;
            } else if (code == 0xB8) {
                seq.time_code = br.get(25);
                seq.closed_gop = uint8_t(br.get(1));
                seq.broken_link = uint8_t(br.get(1));
                present |= 0x20;
            } else if (code == 0x00) {
                if (pic_index == 0) ph = cur;
                pic_index++;
                cur = PicHeader{};
                cur.tref = uint16_t(br.get(10));
                cur.type = uint8_t(br.get(3));
                cur.vbv = uint16_t(br.get(16));
                present |= pic_index == 0 ? 0x100 : 0x1000;
            } else if (code == 0xB7) {
                seq_end = true;
                present |= 0x10;
            }
        }
        if (pic_index == 0) ph = cur;
        seq.retained |= present;
        std::vector<uint8_t> info(kInfoSize, 0);
        put16(info, 0, seq.hsize);
        put16(info, 2, seq.vsize);
        info[4] = seq.aspect;
        info[5] = seq.frc;
        info[6] = seq.progressive_seq;
        info[7] = seq.low_delay;
        info[8] = seq.video_format;
        info[9] = seq.colour_desc;
        info[10] = seq.primaries;
        info[11] = seq.transfer;
        info[12] = seq.matrix;
        put16(info, 14, ph.tref);
        info[18] = ph.type;
        info[20] = ph.structure;
        info[22] = ph.tff;
        info[23] = ph.rff;
        info[24] = ph.progressive_frame;
        put32(info, 28, seq.time_code);
        info[32] = seq.closed_gop;
        info[33] = seq.broken_link;
        put16(info, 34, ph.vbv);
        put16(info, 38, seq.disp_h);
        put16(info, 40, seq.disp_v);
        info[42] = ph.centre_n;
        for (int j = 0; j < 3; j++) {
            put16(info, 44 + 2 * j, ph.fch[j]);
            put16(info, 56 + 2 * j, ph.fcv[j]);
        }
        put32(info, 68, present);
        put32(info, 72, seq.retained);
        info[76] = seq.mpeg1;
        return info;
    }

    uint32_t fmt_key(uint32_t type, uint32_t matrix, uint8_t alpha) { return type | (matrix << 4) | (uint32_t(alpha) << 8); }

    void convert(const media::Frame& f, uint32_t key, uint8_t* dst) {
        uint32_t type = key & 0xF;
        bool bt709 = ((key >> 4) & 0xF) == 1;
        uint8_t alpha = uint8_t(key >> 8);
        int w = f.width, h = f.height;
        if (type == FMT_ARGB || type == FMT_RGBA) {
            media::i420_to_rgb32(f, dst, size_t(w) * 4, type == FMT_ARGB ? media::PixelOrder::ARGB : media::PixelOrder::RGBA, bt709, alpha);
        } else if (type == FMT_YUV420) {
            std::memcpy(dst, f.y.data(), f.y.size());
            std::memcpy(dst + f.y.size(), f.u.data(), f.u.size());
            std::memcpy(dst + f.y.size() + f.u.size(), f.v.data(), f.v.size());
        } else {
            for (int r = 0; r < h; r++) {
                uint8_t* o = dst + size_t(r) * w * 2;
                const uint8_t* y = &f.y[size_t(r) * w];
                const uint8_t* u = &f.u[size_t(r / 2) * (w / 2)];
                const uint8_t* v = &f.v[size_t(r / 2) * (w / 2)];
                for (int x = 0; x < w / 2; x++) {
                    o[4 * x] = u[x];
                    o[4 * x + 1] = y[2 * x];
                    o[4 * x + 2] = v[x];
                    o[4 * x + 3] = y[2 * x + 1];
                }
            }
        }
    }

    static size_t out_bytes(uint32_t key, const media::Frame& f) {
        size_t px = size_t(f.width) * f.height;
        switch (key & 0xF) {
        case FMT_ARGB:
        case FMT_RGBA: return px * 4;
        case FMT_UYVY: return px * 2;
        default: return px * 3 / 2;
        }
    }

    void collect(guest::Thread& t, media::VideoDecoder& dec) {
        media::Frame f;
        while (dec.pop(f)) {
            Picture pic;
            auto it = metas.find(f.time);
            if (it == metas.end() && !metas.empty()) it = metas.begin();
            if (it != metas.end()) {
                pic.meta = std::move(it->second);
                metas.erase(it);
            }
            uint32_t key;
            {
                std::lock_guard lk(m);
                key = last_fmt_key;
            }
            pic.conv.resize(out_bytes(key, f));
            convert(f, key, pic.conv.data());
            pic.conv_key = key;
            int64_t ptime = f.time;
            uint32_t pau = pic.meta.au[0].start;
            pic.frame = std::move(f);
            {
                std::lock_guard lk(m);
                out.push_back(std::move(pic));
            }
            pics++;
            if (verbose()) LOG_INFO("vdec", "handle 0x%x: picture %llu time %lld au 0x%08x", handle, static_cast<unsigned long long>(pics), static_cast<long long>(ptime), pau);
            callback(t, MSG_PICOUT, 0);
        }
    }

    void run(guest::Thread& t) {
        std::unique_ptr<media::VideoDecoder> dec = media::create_decoder(media::Codec::Mpeg2);
        if (dec) LOG_INFO("vdec", "handle 0x%x: %s", handle, dec->describe().c_str());
        else LOG_ERROR("vdec", "handle 0x%x: decoder creation failed", handle);
        for (;;) {
            Cmd c;
            {
                std::unique_lock lk(m);
                cv.wait(lk, [&] { return closing || !cmds.empty(); });
                if (closing) break;
                c = std::move(cmds.front());
                cmds.pop_front();
            }
            if (c.kind == Cmd::StartSeq) {
                if (dec) dec->flush();
                metas.clear();
                pending_field = -1;
                continue;
            }
            if (c.kind == Cmd::EndSeq) {
                if (dec) {
                    dec->drain();
                    collect(t, *dec);
                }
                metas.clear();
                LOG_INFO("vdec", "handle 0x%x: sequence done (%llu AUs, %llu pictures)", handle, static_cast<unsigned long long>(aus), static_cast<unsigned long long>(pics));
                callback(t, MSG_SEQDONE, 0);
                continue;
            }
            {
                std::unique_lock lk(m);
                cv.wait(lk, [&] { return closing || out.size() < kOutCap; });
                if (closing) break;
            }
            PicHeader ph;
            bool seq_end = false;
            uint32_t present = 0;
            std::vector<uint8_t> info = parse(c.data.data(), c.data.size(), ph, seq_end, present);
            bool skip = (c.mode == 1 && ph.type == 3) || (c.mode == 2 && ph.type >= 2);
            if (dec && ph.type && !skip) {
                int64_t time = next_time++;
                if (ph.structure != 3 && pending_field >= 0 && metas.count(pending_field)) {
                    Meta& mm = metas[pending_field];
                    mm.au[1] = c.au;
                    mm.au_num = 2;
                    pending_field = -1;
                } else {
                    Meta mm;
                    mm.au[0] = c.au;
                    mm.info = std::move(info);
                    metas[time] = std::move(mm);
                    pending_field = ph.structure != 3 ? time : -1;
                }
                if (!dec->feed(c.data.data(), c.data.size(), time)) LOG_WARN("vdec", "handle 0x%x: AU %llu rejected by decoder", handle, static_cast<unsigned long long>(aus));
            }
            if (dec) {
                if (seq_end) dec->drain();
                collect(t, *dec);
            }
            aus++;
            if (verbose()) LOG_INFO("vdec", "handle 0x%x: AU %llu at 0x%08x size 0x%x type %u tref %u%s", handle, static_cast<unsigned long long>(aus), c.au.start, c.au.size, ph.type, ph.tref, seq_end ? " seq_end" : "");
            {
                std::lock_guard lk(m);
                inflight--;
            }
            callback(t, MSG_AUDONE, 0);
        }
        dec.reset();
    }
};

std::mutex g_lock;
std::map<uint32_t, std::shared_ptr<Decoder>> g_decoders;
uint32_t g_next_handle = 0x7d000100;

std::shared_ptr<Decoder> get(uint32_t h) {
    std::lock_guard lk(g_lock);
    auto it = g_decoders.find(h);
    return it == g_decoders.end() ? nullptr : it->second;
}

bool codec_ok(uint32_t codec, uint32_t level) { return codec == CODEC_MPEG2 && level <= 3; }

int32_t query_attr(uint32_t codec, uint32_t level, uint32_t attr) {
    if (!attr) return VDEC_ERROR_ARG;
    vm::write32(attr, 0);
    if (!codec_ok(codec, level)) return VDEC_ERROR_ARG;
    vm::write32(attr, kMemSize);
    vm::ptr(attr + 4)[0] = kCmdDepth;
    vm::write32(attr + 8, 0x4890000);
    vm::write32(attr + 12, 0x00020000);
    return CELL_OK;
}

int32_t open(ppu::Context& ctx, uint32_t codec, uint32_t level, uint32_t res, uint32_t cb, uint32_t handle) {
    if (!res || !cb || !handle) return VDEC_ERROR_ARG;
    if (!codec_ok(codec, level)) return VDEC_ERROR_ARG;
    auto d = std::make_shared<Decoder>();
    d->codec = codec;
    d->mem_addr = vm::read32(res);
    d->mem_size = vm::read32(res + 4);
    d->cb_func = vm::read32(cb);
    d->cb_arg = vm::read32(cb + 4);
    int32_t prio = static_cast<int32_t>(vm::read32(res + 8));
    {
        std::lock_guard lk(g_lock);
        d->handle = d->mem_addr && !g_decoders.count(d->mem_addr) ? d->mem_addr : g_next_handle++;
        g_decoders[d->handle] = d;
    }
    std::weak_ptr<Decoder> wd = d;
    d->thread = guest::spawn_host("_libvdec_native", prio, 0x10000, static_cast<uint32_t>(ctx.gpr[13]), [wd](guest::Thread& t) {
        if (auto dd = wd.lock()) dd->run(t);
    });
    vm::write32(handle, d->handle);
    LOG_INFO("vdec", "open handle 0x%x codec %u level %u mem 0x%08x+0x%x cb 0x%08x(0x%x)", d->handle, codec, level, d->mem_addr, d->mem_size, d->cb_func, d->cb_arg);
    return CELL_OK;
}

}

namespace {

struct DumpState {
    std::string dir;
    int every = 30;
    uint32_t count = 0;
    uint32_t w = 0, h = 0;
};

DumpState& dump_state() {
    static DumpState d;
    return d;
}

void tap_open(const uint64_t*, uint64_t result) {
    if (static_cast<int32_t>(result) == 0) dump_state().count = 0;
}

void tap_item(const uint64_t* a, uint64_t result) {
    if (static_cast<int32_t>(result) != 0) return;
    uint32_t item = vm::read32(static_cast<uint32_t>(a[1]));
    uint32_t info = vm::read32(item + 0x48);
    dump_state().w = vm::read16(info);
    dump_state().h = vm::read16(info + 2);
}

void tap_picture(const uint64_t* a, uint64_t result) {
    DumpState& d = dump_state();
    if (static_cast<int32_t>(result) != 0) return;
    uint32_t n = d.count++;
    if (n % d.every) return;
    uint32_t type = vm::read32(static_cast<uint32_t>(a[1]));
    uint32_t buf = static_cast<uint32_t>(a[2]);
    size_t bytes = size_t(d.w) * d.h * (type <= FMT_RGBA ? 4 : type == FMT_UYVY ? 2 : 1);
    if (type == FMT_YUV420) bytes = bytes * 3 / 2;
    if (!buf || !bytes) return;
    char name[512];
    std::snprintf(name, sizeof name, "%s/pic_%05u_%ux%u_f%u.raw", d.dir.c_str(), n, d.w, d.h, type);
    if (FILE* f = std::fopen(name, "wb")) {
        std::fwrite(vm::ptr(buf), 1, bytes, f);
        std::fclose(f);
    }
}

const bool g_dump_taps = [] {
    const char* e = std::getenv("GOWA_VDEC_DUMP");
    if (!e || !*e) return false;
    std::string v = e;
    size_t c = v.find(',');
    dump_state().dir = v.substr(0, c);
    if (c != std::string::npos) dump_state().every = std::max(1, std::atoi(v.c_str() + c + 1));
    static TapReg r1("libvdec", "cellVdecOpenEx", tap_open);
    static TapReg r2("libvdec", "cellVdecGetPicItem", tap_item);
    static TapReg r3("libvdec", "cellVdecGetPicture", tap_picture);
    return true;
}();

}

static ModuleOverride g_vdec_override("libvdec.sprx", hle_enabled);
static ModuleOverride g_smvd2_override("libsmvd2.sprx", hle_enabled);

HLE_FUNC(libvdec, cellVdecQueryAttr) {
    uint32_t type = a32(ctx, 0);
    if (!type) return ret(ctx, VDEC_ERROR_ARG);
    ret(ctx, query_attr(vm::read32(type), vm::read32(type + 4), a32(ctx, 1)));
}

HLE_FUNC(libvdec, cellVdecQueryAttrEx) {
    uint32_t type = a32(ctx, 0);
    if (!type) return ret(ctx, VDEC_ERROR_ARG);
    ret(ctx, query_attr(vm::read32(type), vm::read32(type + 4), a32(ctx, 1)));
}

HLE_FUNC(libvdec, cellVdecOpen) {
    uint32_t type = a32(ctx, 0);
    if (!type) return ret(ctx, VDEC_ERROR_ARG);
    ret(ctx, open(ctx, vm::read32(type), vm::read32(type + 4), a32(ctx, 1), a32(ctx, 2), a32(ctx, 3)));
}

HLE_FUNC(libvdec, cellVdecOpenEx) {
    uint32_t type = a32(ctx, 0);
    if (!type) return ret(ctx, VDEC_ERROR_ARG);
    ret(ctx, open(ctx, vm::read32(type), vm::read32(type + 4), a32(ctx, 1), a32(ctx, 2), a32(ctx, 3)));
}

HLE_FUNC(libvdec, cellVdecClose) {
    auto d = get(a32(ctx, 0));
    if (!d) return ret(ctx, VDEC_ERROR_ARG);
    {
        std::lock_guard lk(d->m);
        d->closing = true;
    }
    d->cv.notify_all();
    guest::join_host(d->thread);
    {
        std::lock_guard lk(g_lock);
        g_decoders.erase(d->handle);
    }
    LOG_INFO("vdec", "close handle 0x%x (%llu AUs, %llu pictures)", d->handle, static_cast<unsigned long long>(d->aus), static_cast<unsigned long long>(d->pics));
    ret(ctx, CELL_OK);
}

HLE_FUNC(libvdec, cellVdecStartSeq) {
    auto d = get(a32(ctx, 0));
    if (!d) return ret(ctx, VDEC_ERROR_ARG);
    {
        std::lock_guard lk(d->m);
        if (d->started) return ret(ctx, VDEC_ERROR_SEQ);
        d->started = true;
        d->out.clear();
        Cmd c;
        c.kind = Cmd::StartSeq;
        d->cmds.push_back(std::move(c));
    }
    d->cv.notify_all();
    ret(ctx, CELL_OK);
}

HLE_FUNC(libvdec, cellVdecEndSeq) {
    auto d = get(a32(ctx, 0));
    if (!d) return ret(ctx, VDEC_ERROR_ARG);
    {
        std::lock_guard lk(d->m);
        if (!d->started) return ret(ctx, VDEC_ERROR_SEQ);
        d->started = false;
        Cmd c;
        c.kind = Cmd::EndSeq;
        d->cmds.push_back(std::move(c));
    }
    d->cv.notify_all();
    ret(ctx, CELL_OK);
}

HLE_FUNC(libvdec, cellVdecDecodeAu) {
    auto d = get(a32(ctx, 0));
    uint32_t mode = a32(ctx, 1);
    uint32_t au = a32(ctx, 2);
    if (!d || !au || mode > 2) return ret(ctx, VDEC_ERROR_ARG);
    Cmd c;
    c.kind = Cmd::Au;
    c.mode = mode;
    c.au.start = vm::read32(au);
    c.au.size = vm::read32(au + 4);
    c.au.pts_hi = vm::read32(au + 8);
    c.au.pts_lo = vm::read32(au + 12);
    c.au.dts_hi = vm::read32(au + 16);
    c.au.dts_lo = vm::read32(au + 20);
    c.au.user = vm::read64(au + 24);
    if (!c.au.start || !c.au.size) return ret(ctx, VDEC_ERROR_ARG);
    {
        std::lock_guard lk(d->m);
        if (!d->started) return ret(ctx, VDEC_ERROR_SEQ);
        if (d->inflight >= kMaxInflight) return ret(ctx, VDEC_ERROR_BUSY);
        d->inflight++;
    }
    c.data.assign(vm::ptr(c.au.start), vm::ptr(c.au.start) + c.au.size);
    {
        std::lock_guard lk(d->m);
        d->cmds.push_back(std::move(c));
    }
    d->cv.notify_all();
    ret(ctx, CELL_OK);
}

HLE_FUNC(libvdec, cellVdecGetPicItem) {
    auto d = get(a32(ctx, 0));
    uint32_t pp = a32(ctx, 1);
    if (!d || !pp) return ret(ctx, VDEC_ERROR_ARG);
    std::lock_guard lk(d->m);
    Picture* pic = nullptr;
    for (auto& p : d->out) {
        if (!p.item_given) {
            pic = &p;
            break;
        }
    }
    if (!pic) return ret(ctx, VDEC_ERROR_EMPTY);
    pic->item_given = true;
    uint32_t slots = d->mem_size >= kSlotSize ? std::min<uint32_t>(kSlots, d->mem_size / kSlotSize) : 0;
    if (!d->mem_addr || !slots) return ret(ctx, VDEC_ERROR_FATAL);
    uint32_t item = d->mem_addr + (d->slot++ % slots) * kSlotSize;
    uint32_t info = item + kItemSize;
    std::memset(vm::ptr(item), 0, kSlotSize);
    const Meta& mm = pic->meta;
    vm::write32(item + 0x00, d->codec);
    vm::write32(item + 0x04, mm.au[0].start);
    vm::write32(item + 0x08, mm.au[0].size + (mm.au_num > 1 ? mm.au[1].size : 0));
    vm::ptr(item + 0x0c)[0] = mm.au_num;
    for (int k = 0; k < 2; k++) {
        const AuInfo& a = k < mm.au_num ? mm.au[k] : AuInfo{};
        vm::write32(item + 0x10 + 8 * k, a.pts_hi);
        vm::write32(item + 0x14 + 8 * k, a.pts_lo);
        vm::write32(item + 0x20 + 8 * k, a.dts_hi);
        vm::write32(item + 0x24 + 8 * k, a.dts_lo);
        vm::write64(item + 0x30 + 8 * k, k < mm.au_num ? a.user : 0);
    }
    vm::write32(item + 0x40, 0);
    vm::write32(item + 0x44, 0);
    vm::write32(item + 0x48, info);
    if (!mm.info.empty()) std::memcpy(vm::ptr(info), mm.info.data(), std::min<size_t>(mm.info.size(), kInfoSize));
    vm::write32(pp, item);
    ret(ctx, CELL_OK);
}

HLE_FUNC(libvdec, cellVdecGetPicture) {
    auto d = get(a32(ctx, 0));
    uint32_t fmt = a32(ctx, 1);
    uint32_t buf = a32(ctx, 2);
    if (!d || !fmt) return ret(ctx, VDEC_ERROR_ARG);
    uint32_t type = vm::read32(fmt);
    uint32_t matrix = vm::read32(fmt + 4);
    uint8_t alpha = vm::ptr(fmt + 8)[0];
    if (type > FMT_YUV420 || (type <= FMT_RGBA && matrix > 1)) return ret(ctx, VDEC_ERROR_ARG);
    uint32_t key = d->fmt_key(type, matrix, alpha);
    Picture pic;
    {
        std::lock_guard lk(d->m);
        if (d->out.empty()) return ret(ctx, VDEC_ERROR_EMPTY);
        pic = std::move(d->out.front());
        d->out.pop_front();
        if (d->last_fmt_key != key) {
            d->last_fmt_key = key;
            LOG_INFO("vdec", "handle 0x%x: picture format %u matrix %u alpha 0x%02x, %dx%d", d->handle, type, matrix, alpha, pic.frame.width, pic.frame.height);
        }
    }
    d->cv.notify_all();
    if (buf) {
        size_t n = Decoder::out_bytes(key, pic.frame);
        vm::host_write(buf, n);
        if (pic.conv_key == key) std::memcpy(vm::ptr(buf), pic.conv.data(), n);
        else d->convert(pic.frame, key, vm::ptr(buf));
    }
    ret(ctx, CELL_OK);
}

HLE_FUNC(libvdec, cellVdecSetFrameRate) {
    ret(ctx, get(a32(ctx, 0)) ? CELL_OK : VDEC_ERROR_ARG);
}

}
