#pragma once

#include <cstdint>
#include <vector>

namespace rm {

// GPU-accelerated screen capture via DXGI Desktop Duplication.
//
// Replaces the old GDI StretchBlt path. Captures the primary output, exposes
// changed/idle detection (so a static screen costs almost no CPU), and returns
// tightly-packed top-down BGRA cropped to even dimensions (required by the NV12
// H.264 encoder).
class ScreenDuplicator {
public:
    ScreenDuplicator() = default;
    ~ScreenDuplicator();

    ScreenDuplicator(const ScreenDuplicator&) = delete;
    ScreenDuplicator& operator=(const ScreenDuplicator&) = delete;

    bool initialize();
    void shutdown();

    // Result codes for acquire().
    enum class Result {
        Frame,    // a new frame was written to out_bgra
        Idle,     // no desktop change within the timeout (nothing written)
        Lost,     // duplication invalidated (caller should shutdown()+initialize())
        Error     // unrecoverable
    };

    // Captures one frame. out_bgra is tightly packed BGRA (top-down),
    // out_width x out_height, both even. timeout_ms bounds the wait.
    Result acquire(
        std::vector<unsigned char>& out_bgra,
        int& out_width,
        int& out_height,
        int timeout_ms
    );

    // Full (un-cropped) desktop dimensions, for input coordinate mapping.
    int desktop_width() const { return desktop_width_; }
    int desktop_height() const { return desktop_height_; }

    // HRESULT of the last DXGI failure (0 if DXGI succeeded), for diagnostics.
    long last_error() const { return last_error_; }

    // True when DXGI Desktop Duplication was unavailable and the slower GDI
    // BitBlt fallback is in use.
    bool using_gdi() const { return use_gdi_; }

private:
    bool initialize_dxgi();
    bool create_staging(unsigned int width, unsigned int height, int format);

    bool init_gdi();
    Result acquire_gdi(
        std::vector<unsigned char>& out_bgra,
        int& out_width,
        int& out_height,
        int timeout_ms
    );

    long last_error_ = 0;

    // GDI fallback state.
    bool use_gdi_ = false;
    std::vector<unsigned char> prev_frame_;
    unsigned long long last_tick_ = 0;

    void* device_ = nullptr;       // ID3D11Device*
    void* context_ = nullptr;      // ID3D11DeviceContext*
    void* duplication_ = nullptr;  // IDXGIOutputDuplication*
    void* staging_ = nullptr;      // ID3D11Texture2D*

    unsigned int staging_width_ = 0;
    unsigned int staging_height_ = 0;
    int staging_format_ = 0;

    int desktop_width_ = 0;
    int desktop_height_ = 0;

    bool delivered_first_ = false;
};

}
