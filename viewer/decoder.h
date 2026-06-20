#pragma once

#include <cstdint>
#include <vector>

namespace rm {

struct DecodedFrame {
    std::vector<unsigned char> bgra; // tightly packed, top-down
    int width = 0;
    int height = 0;
};

// H.264 decoder built on the Microsoft Media Foundation H.264 decoder MFT.
// Accepts Annex-B access units and produces BGRA frames for rendering.
class H264Decoder {
public:
    H264Decoder() = default;
    ~H264Decoder();

    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    bool initialize();
    void shutdown();

    // Feeds one Annex-B access unit. Appends 0+ decoded frames to out_frames.
    bool decode(const unsigned char* data, size_t size, std::vector<DecodedFrame>& out_frames);

private:
    bool negotiate_output_type();
    bool drain_output(std::vector<DecodedFrame>& out_frames);
    void nv12_to_bgra(const unsigned char* nv12, int stride, DecodedFrame& frame);

    void* transform_ = nullptr; // IMFTransform*

    int width_ = 0;
    int height_ = 0;
    int stride_ = 0;
    bool output_ready_ = false;
    bool provides_samples_ = false;
    unsigned long output_size_ = 0;
    long long pts_ = 0;
    long long frame_duration_ = 333333; // ~30fps in 100ns units
};

}
