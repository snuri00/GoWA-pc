#if !defined(_WIN32)
#include "runtime/video_decoder.h"
#include "runtime/log.h"

#include <cstring>
#include <deque>

#ifdef GOWA_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
}
#endif

namespace media {

#ifdef GOWA_HAVE_FFMPEG

namespace {

class FfDecoder : public VideoDecoder {
public:
    bool open(Codec codec) {
        const AVCodec* c = avcodec_find_decoder(codec == Codec::Mpeg2 ? AV_CODEC_ID_MPEG2VIDEO : AV_CODEC_ID_H264);
        if (!c) {
            LOG_WARN("media", "libavcodec has no %s decoder", codec == Codec::Mpeg2 ? "MPEG-2" : "H.264");
            return false;
        }
        name_ = std::string("libavcodec ") + c->name;
        ctx_ = avcodec_alloc_context3(c);
        if (!ctx_) return false;
        ctx_->thread_count = 0;
        ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        if (avcodec_open2(ctx_, c, nullptr) < 0) {
            LOG_WARN("media", "%s: avcodec_open2 failed", name_.c_str());
            return false;
        }
        pkt_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        return pkt_ && frame_;
    }

    ~FfDecoder() override {
        av_frame_free(&frame_);
        av_packet_free(&pkt_);
        avcodec_free_context(&ctx_);
    }

    bool feed(const uint8_t* data, size_t size, int64_t time) override {
        if (av_new_packet(pkt_, static_cast<int>(size)) < 0) return false;
        std::memcpy(pkt_->data, data, size);
        pkt_->pts = time >= 0 ? time : AV_NOPTS_VALUE;
        int r = avcodec_send_packet(ctx_, pkt_);
        av_packet_unref(pkt_);
        if (r < 0 && r != AVERROR(EAGAIN)) {
            LOG_WARN("media", "%s: send_packet failed %d", name_.c_str(), r);
            return false;
        }
        return pull();
    }

    bool drain() override {
        avcodec_send_packet(ctx_, nullptr);
        bool ok = pull();
        avcodec_flush_buffers(ctx_);
        return ok;
    }

    void flush() override {
        avcodec_flush_buffers(ctx_);
        frames_.clear();
    }

    bool pop(Frame& out) override {
        if (frames_.empty()) return false;
        out = std::move(frames_.front());
        frames_.pop_front();
        return true;
    }

    size_t pending() const override { return frames_.size(); }

    std::string describe() const override { return name_; }

private:
    bool pull() {
        for (;;) {
            int r = avcodec_receive_frame(ctx_, frame_);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
            if (r < 0) return false;
            take();
            av_frame_unref(frame_);
        }
    }

    void take() {
        if (frame_->format != AV_PIX_FMT_YUV420P && frame_->format != AV_PIX_FMT_YUVJ420P) {
            static bool warned = false;
            if (!warned) LOG_WARN("media", "%s: unsupported pixel format %d", name_.c_str(), frame_->format);
            warned = true;
            return;
        }
        Frame f;
        f.width = frame_->width & ~1;
        f.height = frame_->height & ~1;
        f.time = frame_->pts == AV_NOPTS_VALUE ? -1 : frame_->pts;
        int w = f.width, h = f.height, cw = w / 2, ch = h / 2;
        f.y.resize(size_t(w) * h);
        f.u.resize(size_t(cw) * ch);
        f.v.resize(size_t(cw) * ch);
        for (int r = 0; r < h; r++) std::memcpy(&f.y[size_t(r) * w], frame_->data[0] + size_t(r) * frame_->linesize[0], w);
        for (int r = 0; r < ch; r++) {
            std::memcpy(&f.u[size_t(r) * cw], frame_->data[1] + size_t(r) * frame_->linesize[1], cw);
            std::memcpy(&f.v[size_t(r) * cw], frame_->data[2] + size_t(r) * frame_->linesize[2], cw);
        }
        frames_.push_back(std::move(f));
    }

    AVCodecContext* ctx_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* frame_ = nullptr;
    std::string name_;
    std::deque<Frame> frames_;
};

}

std::unique_ptr<VideoDecoder> create_decoder(Codec codec) {
    auto d = std::make_unique<FfDecoder>();
    if (!d->open(codec)) return nullptr;
    return d;
}

#else

std::unique_ptr<VideoDecoder> create_decoder(Codec) { return nullptr; }

#endif

}
#endif
