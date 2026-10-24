#include "runtime/video_decoder.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> read_all(const char* path) {
    std::vector<uint8_t> v;
    FILE* f = std::fopen(path, "rb");
    if (!f) return v;
    std::fseek(f, 0, SEEK_END);
    v.resize(static_cast<size_t>(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
    std::fclose(f);
    return v;
}

std::vector<std::pair<size_t, size_t>> split_aus(const std::vector<uint8_t>& d) {
    std::vector<std::pair<size_t, size_t>> aus;
    size_t pos = 0;
    while (pos < d.size()) {
        size_t start = SIZE_MAX, end = 0;
        bool pic = false;
        size_t i = pos;
        while (i + 4 <= d.size()) {
            if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
                uint8_t c = d[i + 3];
                if (c == 0xb7 || c == 0xb8 || c == 0x00 || c == 0xb3) {
                    if (pic) {
                        end = c == 0xb7 ? i + 4 : i;
                        break;
                    }
                    if (c == 0x00) pic = true;
                    if (start == SIZE_MAX) start = i;
                }
                i += 4;
            } else {
                i++;
            }
        }
        if (start == SIZE_MAX) break;
        if (!end) end = d.size();
        aus.push_back({start, end - start});
        pos = end;
    }
    return aus;
}

void write_ppm(const std::string& path, const media::Frame& f) {
    std::vector<uint8_t> argb(size_t(f.width) * f.height * 4);
    media::i420_to_rgb32(f, argb.data(), size_t(f.width) * 4, media::PixelOrder::ARGB, false, 0xff);
    FILE* o = std::fopen(path.c_str(), "wb");
    if (!o) return;
    std::fprintf(o, "P6\n%d %d\n255\n", f.width, f.height);
    for (size_t i = 0; i < size_t(f.width) * f.height; i++) std::fwrite(&argb[i * 4 + 1], 1, 3, o);
    std::fclose(o);
}

}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: vdec_test <file.m2v> [dump_dir] [dump_every] [max_aus]\n");
        return 1;
    }
    auto data = read_all(argv[1]);
    std::string dump = argc > 2 ? argv[2] : "";
    int every = argc > 3 ? std::atoi(argv[3]) : 100;
    size_t max_aus = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : SIZE_MAX;
    auto aus = split_aus(data);
    std::printf("%zu bytes, %zu AUs\n", data.size(), aus.size());
    auto dec = media::create_decoder(media::Codec::Mpeg2);
    if (!dec) {
        std::printf("no decoder\n");
        return 2;
    }
    std::printf("decoder %s\n", dec->describe().c_str());
    auto t0 = std::chrono::steady_clock::now();
    size_t frames = 0;
    double conv_ms = 0;
    std::vector<uint8_t> argb;
    media::Frame f;
    auto take = [&] {
        while (dec->pop(f)) {
            auto c0 = std::chrono::steady_clock::now();
            argb.resize(size_t(f.width) * f.height * 4);
            media::i420_to_rgb32(f, argb.data(), size_t(f.width) * 4, media::PixelOrder::ARGB, false, 0xff);
            conv_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
            if (!dump.empty() && frames % every == 0) {
                char name[64];
                std::snprintf(name, sizeof name, "/f%05zu_t%lld.ppm", frames, static_cast<long long>(f.time));
                write_ppm(dump + name, f);
            }
            if (std::getenv("VDEC_TIMES") && frames < 40) std::printf("out %zu t %lld\n", frames, static_cast<long long>(f.time));
            frames++;
        }
    };
    for (size_t i = 0; i < aus.size() && i < max_aus; i++) {
        if (std::getenv("VDEC_TIMES") && i < 40) {
            size_t o = aus[i].first, e = o + aus[i].second;
            for (size_t k = o; k + 6 <= e; k++) if (data[k] == 0 && data[k + 1] == 0 && data[k + 2] == 1 && data[k + 3] == 0) { std::printf("au %zu type %d tref %d\n", i, (data[k + 5] >> 3) & 7, (data[k + 4] << 2) | (data[k + 5] >> 6)); break; }
        }
        if (!dec->feed(&data[aus[i].first], aus[i].second, static_cast<int64_t>(i))) std::printf("feed failed at AU %zu\n", i);
        take();
    }
    dec->drain();
    take();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%zu frames %dx%d in %.1f ms (%.2f ms/frame, convert %.2f ms/frame)\n", frames, f.width, f.height, ms, frames ? ms / frames : 0.0, frames ? conv_ms / frames : 0.0);
    return 0;
}
