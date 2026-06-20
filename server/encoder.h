#pragma once

#include <cstdint>
#include <vector>

namespace rm {

// H.264 encoder built on the Microsoft Media Foundation H.264 encoder MFT
// (synchronous software transform). Takes BGRA frames, converts to NV12, and
// emits Annex-B access units. Temporal (inter-frame) compression means a static
// or slightly-changed screen produces tiny P-frames -- this is what replaces
// per-frame JPEG and per-tile dirty-rect encoding.
//
// NOTE: a hardware async MFT (NVENC / QuickSync) can be swapped in here without
// touching callers; only the MFT creation + drive loop would change.
class H264Encoder {
public:
    H264Encoder() = default;
    ~H264Encoder();

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    // width/height must be even.
    bool initialize(int width, int height, int fps, int bitrate_bps);
    void shutdown();

    // Encodes one tightly-packed top-down BGRA frame (width*height*4 bytes).
    // Appends 0+ Annex-B access units to out_units; out_keyframe[i] marks
    // whether unit i is an IDR (keyframe).
    bool encode(
        const unsigned char* bgra,
        bool force_keyframe,
        std::vector<std::vector<unsigned char>>& out_units,
        std::vector<bool>& out_keyframe
    );

    int width() const { return width_; }
    int height() const { return height_; }

private:
    bool configure_types();
    void apply_codec_settings();
    void bgra_to_nv12(const unsigned char* bgra);
    bool drain_output(
        std::vector<std::vector<unsigned char>>& out_units,
        std::vector<bool>& out_keyframe
    );
    void capture_sequence_header();

    void* transform_ = nullptr;  // IMFTransform*
    void* codec_api_ = nullptr;  // ICodecAPI*

    int width_ = 0;
    int height_ = 0;
    int fps_ = 30;
    int bitrate_ = 0;

    long long pts_ = 0;
    long long frame_duration_ = 0;

    bool provides_samples_ = false;
    unsigned long output_size_ = 0;

    std::vector<unsigned char> nv12_;
    std::vector<unsigned char> sequence_header_; // SPS/PPS (Annex B)
};

}
