#ifdef _WIN32
#include "runtime/video_decoder.h"
#include "runtime/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace media {

namespace {

std::once_flag g_startup;
bool g_mf_ok = false;

bool startup() {
    std::call_once(g_startup, [] {
        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
        g_mf_ok = SUCCEEDED(hr);
        if (!g_mf_ok) LOG_ERROR("media", "MFStartup failed 0x%08lx", static_cast<unsigned long>(hr));
    });
    return g_mf_ok;
}

enum class Layout { I420, YV12, NV12 };

class MfDecoder : public VideoDecoder {
public:
    bool open(Codec codec) {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com_ = SUCCEEDED(hr);
        if (!startup()) return false;
        GUID sub = codec == Codec::Mpeg2 ? MFVideoFormat_MPEG2 : MFVideoFormat_H264;
        const char* pick = std::getenv("GOWA_VDEC_MFT");
        if (codec == Codec::Mpeg2 && (!pick || std::strcmp(pick, "builtin") == 0)) {
            static const CLSID clsid = {0x2D709E52, 0x123F, 0x49B5, {0x9C, 0xBC, 0x9A, 0xF5, 0xCD, 0xE2, 0x8F, 0xB9}};
            ComPtr<IMFTransform> t;
            HRESULT hc = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&t));
            if (SUCCEEDED(hc)) {
                name_ = "Microsoft MPEG Video Decoder MFT";
                mft_ = t;
            } else {
                LOG_INFO("media", "built-in MPEG-2 decoder unavailable (0x%08lx)", static_cast<unsigned long>(hc));
            }
        }
        if (!mft_) {
            MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, sub};
            IMFActivate** acts = nullptr;
            UINT32 n = 0;
            hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, nullptr, &acts, &n);
            for (UINT32 i = 0; SUCCEEDED(hr) && i < n && !mft_; i++) {
                WCHAR name[256] = {};
                UINT32 len = 0;
                char buf[256] = "?";
                if (SUCCEEDED(acts[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 256, &len))) WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, sizeof buf, nullptr, nullptr);
                if (pick && std::strcmp(pick, "builtin") != 0 && std::strstr(buf, pick) == nullptr) continue;
                ComPtr<IMFTransform> t;
                if (SUCCEEDED(acts[i]->ActivateObject(IID_PPV_ARGS(&t)))) {
                    name_ = buf;
                    mft_ = t;
                }
            }
            for (UINT32 i = 0; SUCCEEDED(hr) && i < n; i++) acts[i]->Release();
            if (acts) CoTaskMemFree(acts);
        }
        if (!mft_) {
            LOG_ERROR("media", "no Media Foundation decoder for %s", codec == Codec::Mpeg2 ? "MPEG-2" : "H.264");
            return false;
        }
        if (!mft_) return false;
        ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(mft_->GetAttributes(&attrs)) && attrs) {
            const char* ll = std::getenv("GOWA_VDEC_LOWLAT");
            if (!ll || ll[0] != '0') attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        }
        ComPtr<IMFMediaType> it;
        MFCreateMediaType(&it);
        it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        it->SetGUID(MF_MT_SUBTYPE, sub);
        hr = mft_->SetInputType(0, it.Get(), 0);
        if (FAILED(hr)) {
            LOG_ERROR("media", "%s: SetInputType failed 0x%08lx", name_.c_str(), static_cast<unsigned long>(hr));
            return false;
        }
        if (!negotiate_output()) return false;
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    ~MfDecoder() override {
        if (mft_) {
            mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        mft_.Reset();
        if (com_) CoUninitialize();
    }

    bool feed(const uint8_t* data, size_t size, int64_t time) override {
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(size), &buf))) return false;
        BYTE* p = nullptr;
        buf->Lock(&p, nullptr, nullptr);
        std::memcpy(p, data, size);
        buf->Unlock();
        buf->SetCurrentLength(static_cast<DWORD>(size));
        ComPtr<IMFSample> s;
        MFCreateSample(&s);
        s->AddBuffer(buf.Get());
        if (time >= 0) {
            s->SetSampleTime(time);
            s->SetSampleDuration(1);
        }
        for (int tries = 0; tries < 64; tries++) {
            HRESULT hr = mft_->ProcessInput(0, s.Get(), 0);
            if (hr == MF_E_NOTACCEPTING) {
                if (!pull()) return false;
                continue;
            }
            if (FAILED(hr)) {
                LOG_WARN("media", "%s: ProcessInput failed 0x%08lx", name_.c_str(), static_cast<unsigned long>(hr));
                return false;
            }
            return pull();
        }
        return false;
    }

    bool drain() override {
        mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        bool ok = pull();
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return ok;
    }

    void flush() override {
        mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        frames_.clear();
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }

    bool pop(Frame& out) override {
        if (frames_.empty()) return false;
        out = std::move(frames_.front());
        frames_.pop_front();
        return true;
    }

    size_t pending() const override { return frames_.size(); }

    std::string describe() const override {
        const char* l = layout_ == Layout::NV12 ? "NV12" : layout_ == Layout::YV12 ? "YV12" : "I420";
        return name_ + " (" + l + ")";
    }

private:
    bool negotiate_output() {
        static const struct { GUID g; Layout l; } prefs[] = {{MFVideoFormat_IYUV, Layout::I420}, {MFVideoFormat_I420, Layout::I420}, {MFVideoFormat_YV12, Layout::YV12}, {MFVideoFormat_NV12, Layout::NV12}};
        for (const auto& pref : prefs) {
            for (DWORD i = 0;; i++) {
                ComPtr<IMFMediaType> t;
                HRESULT hr = mft_->GetOutputAvailableType(0, i, &t);
                if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) break;
                GUID sub{};
                t->GetGUID(MF_MT_SUBTYPE, &sub);
                if (sub != pref.g) continue;
                if (FAILED(mft_->SetOutputType(0, t.Get(), 0))) continue;
                layout_ = pref.l;
                UINT32 w = 0, h = 0;
                MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
                plane_w_ = static_cast<int>(w);
                plane_h_ = static_cast<int>(h);
                vis_w_ = plane_w_;
                vis_h_ = plane_h_;
                MFVideoArea area{};
                if (SUCCEEDED(t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof area, nullptr)) && area.Area.cx > 0 && area.Area.cy > 0) {
                    vis_w_ = area.Area.cx;
                    vis_h_ = area.Area.cy;
                }
                UINT32 stride = 0;
                default_stride_ = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? static_cast<int>(static_cast<int32_t>(stride)) : 0;
                MFT_OUTPUT_STREAM_INFO si{};
                mft_->GetOutputStreamInfo(0, &si);
                provides_ = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
                out_size_ = si.cbSize;
                return true;
            }
        }
        LOG_ERROR("media", "%s: no supported output type", name_.c_str());
        return false;
    }

    bool pull() {
        for (;;) {
            ComPtr<IMFSample> own;
            MFT_OUTPUT_DATA_BUFFER ob{};
            if (!provides_) {
                ComPtr<IMFMediaBuffer> b;
                if (FAILED(MFCreateMemoryBuffer(out_size_ ? out_size_ : static_cast<DWORD>(plane_w_ * plane_h_ * 2), &b))) return false;
                MFCreateSample(&own);
                own->AddBuffer(b.Get());
                ob.pSample = own.Get();
            }
            DWORD status = 0;
            HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
            if (ob.pEvents) ob.pEvents->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (!negotiate_output()) return false;
                continue;
            }
            if (FAILED(hr)) {
                LOG_WARN("media", "%s: ProcessOutput failed 0x%08lx", name_.c_str(), static_cast<unsigned long>(hr));
                return false;
            }
            IMFSample* s = ob.pSample;
            if (s) {
                take(s);
                if (provides_) s->Release();
            }
        }
    }

    void take(IMFSample* s) {
        Frame f;
        f.width = vis_w_ & ~1;
        f.height = vis_h_ & ~1;
        LONGLONG t = -1;
        if (FAILED(s->GetSampleTime(&t))) t = -1;
        f.time = t;
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(s->ConvertToContiguousBuffer(&buf))) return;
        BYTE* base = nullptr;
        LONG pitch = 0;
        ComPtr<IMF2DBuffer> b2;
        bool locked2d = false;
        if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&base, &pitch))) {
            locked2d = true;
        } else {
            DWORD max = 0, cur = 0;
            if (FAILED(buf->Lock(&base, &max, &cur))) return;
            pitch = default_stride_ > 0 ? default_stride_ : plane_w_;
        }
        int w = f.width, h = f.height, cw = w / 2, ch = h / 2;
        f.y.resize(size_t(w) * h);
        f.u.resize(size_t(cw) * ch);
        f.v.resize(size_t(cw) * ch);
        const uint8_t* y = base;
        for (int r = 0; r < h; r++) std::memcpy(&f.y[size_t(r) * w], y + size_t(r) * pitch, w);
        const uint8_t* c = base + size_t(pitch) * plane_h_;
        if (layout_ == Layout::NV12) {
            for (int r = 0; r < ch; r++) {
                const uint8_t* src = c + size_t(r) * pitch;
                uint8_t* du = &f.u[size_t(r) * cw];
                uint8_t* dv = &f.v[size_t(r) * cw];
                for (int x = 0; x < cw; x++) {
                    du[x] = src[2 * x];
                    dv[x] = src[2 * x + 1];
                }
            }
        } else {
            int cp = pitch / 2;
            const uint8_t* p1 = c;
            const uint8_t* p2 = c + size_t(cp) * (plane_h_ / 2);
            const uint8_t* pu = layout_ == Layout::I420 ? p1 : p2;
            const uint8_t* pv = layout_ == Layout::I420 ? p2 : p1;
            for (int r = 0; r < ch; r++) {
                std::memcpy(&f.u[size_t(r) * cw], pu + size_t(r) * cp, cw);
                std::memcpy(&f.v[size_t(r) * cw], pv + size_t(r) * cp, cw);
            }
        }
        if (locked2d) b2->Unlock2D();
        else buf->Unlock();
        frames_.push_back(std::move(f));
    }

    ComPtr<IMFTransform> mft_;
    std::string name_ = "?";
    bool com_ = false;
    Layout layout_ = Layout::I420;
    int plane_w_ = 0, plane_h_ = 0, vis_w_ = 0, vis_h_ = 0, default_stride_ = 0;
    bool provides_ = false;
    DWORD out_size_ = 0;
    std::deque<Frame> frames_;
};

}

std::unique_ptr<VideoDecoder> create_decoder(Codec codec) {
    auto d = std::make_unique<MfDecoder>();
    if (!d->open(codec)) return nullptr;
    return d;
}

}
#endif
