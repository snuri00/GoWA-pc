#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace media {

enum class Codec { Mpeg2, H264 };

struct Frame {
    int width = 0;
    int height = 0;
    int64_t time = -1;
    std::vector<uint8_t> y, u, v;
};

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;
    virtual bool feed(const uint8_t* data, size_t size, int64_t time) = 0;
    virtual bool drain() = 0;
    virtual void flush() = 0;
    virtual bool pop(Frame& out) = 0;
    virtual size_t pending() const = 0;
    virtual std::string describe() const = 0;
};

std::unique_ptr<VideoDecoder> create_decoder(Codec codec);

enum class PixelOrder { ARGB, RGBA };

void i420_to_rgb32(const Frame& f, uint8_t* dst, size_t pitch, PixelOrder order, bool bt709, uint8_t alpha);

}
