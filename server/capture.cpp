#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "capture.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstring>

namespace rm {

namespace {

template <typename T>
void release_com(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

} // namespace

ScreenDuplicator::~ScreenDuplicator() {
    shutdown();
}

void ScreenDuplicator::shutdown() {
    auto staging = static_cast<ID3D11Texture2D*>(staging_);
    auto duplication = static_cast<IDXGIOutputDuplication*>(duplication_);
    auto context = static_cast<ID3D11DeviceContext*>(context_);
    auto device = static_cast<ID3D11Device*>(device_);

    release_com(staging);
    release_com(duplication);
    release_com(context);
    release_com(device);

    staging_ = nullptr;
    duplication_ = nullptr;
    context_ = nullptr;
    device_ = nullptr;

    staging_width_ = 0;
    staging_height_ = 0;
    staging_format_ = 0;
    delivered_first_ = false;

    use_gdi_ = false;
    prev_frame_.clear();
    last_tick_ = 0;
}

bool ScreenDuplicator::initialize() {
    if (!initialize_dxgi()) {
        // DXGI Desktop Duplication is unavailable on this machine/session
        // (hybrid GPU, RDP, DXGI_ERROR_UNSUPPORTED, ...). Fall back to GDI so
        // the product still works, just with higher CPU cost.
        return init_gdi();
    }
    return true;
}

bool ScreenDuplicator::initialize_dxgi() {
    shutdown();
    last_error_ = 0;

    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        last_error_ = hr;
        return false;
    }

    // Pick the output (monitor) actually attached to the desktop, preferring the
    // primary (origin 0,0). On hybrid-GPU laptops the high-performance adapter
    // often has NO attached outputs, so we must create the D3D11 device on the
    // adapter that owns the chosen output -- not just the default adapter.
    IDXGIAdapter1* best_adapter = nullptr;
    IDXGIOutput* best_output = nullptr;
    DXGI_OUTPUT_DESC best_desc{};
    bool best_is_primary = false;

    IDXGIAdapter1* adapter = nullptr;
    for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
        IDXGIOutput* output = nullptr;
        for (UINT oi = 0; adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; ++oi) {
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop) {
                bool primary = desc.DesktopCoordinates.left == 0 &&
                               desc.DesktopCoordinates.top == 0;

                if (best_output == nullptr || (primary && !best_is_primary)) {
                    release_com(best_output);
                    release_com(best_adapter);

                    best_output = output;
                    best_output->AddRef();
                    best_adapter = adapter;
                    best_adapter->AddRef();
                    best_desc = desc;
                    best_is_primary = primary;
                }
            }
            release_com(output);
        }
        release_com(adapter);
    }

    release_com(factory);

    if (!best_output || !best_adapter) {
        release_com(best_output);
        release_com(best_adapter);
        last_error_ = DXGI_ERROR_NOT_FOUND;
        return false;
    }

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    D3D_FEATURE_LEVEL obtained_level{};

    // An explicit adapter requires DRIVER_TYPE_UNKNOWN.
    hr = D3D11CreateDevice(
        best_adapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        feature_levels,
        ARRAYSIZE(feature_levels),
        D3D11_SDK_VERSION,
        &device,
        &obtained_level,
        &context
    );

    if (FAILED(hr)) {
        last_error_ = hr;
        release_com(best_output);
        release_com(best_adapter);
        return false;
    }

    IDXGIOutput1* output1 = nullptr;
    IDXGIOutputDuplication* duplication = nullptr;

    hr = best_output->QueryInterface(IID_PPV_ARGS(&output1));
    if (SUCCEEDED(hr)) {
        hr = output1->DuplicateOutput(device, &duplication);
    }

    release_com(output1);
    release_com(best_output);
    release_com(best_adapter);

    if (FAILED(hr)) {
        last_error_ = hr;
        release_com(context);
        release_com(device);
        return false;
    }

    desktop_width_ = best_desc.DesktopCoordinates.right - best_desc.DesktopCoordinates.left;
    desktop_height_ = best_desc.DesktopCoordinates.bottom - best_desc.DesktopCoordinates.top;

    device_ = device;
    context_ = context;
    duplication_ = duplication;
    return true;
}

bool ScreenDuplicator::create_staging(unsigned int width, unsigned int height, int format) {
    auto device = static_cast<ID3D11Device*>(device_);
    auto staging = static_cast<ID3D11Texture2D*>(staging_);

    release_com(staging);
    staging_ = nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = static_cast<DXGI_FORMAT>(format);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    ID3D11Texture2D* new_staging = nullptr;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &new_staging);
    if (FAILED(hr)) {
        return false;
    }

    staging_ = new_staging;
    staging_width_ = width;
    staging_height_ = height;
    staging_format_ = format;
    return true;
}

ScreenDuplicator::Result ScreenDuplicator::acquire(
    std::vector<unsigned char>& out_bgra,
    int& out_width,
    int& out_height,
    int timeout_ms
) {
    if (use_gdi_) {
        return acquire_gdi(out_bgra, out_width, out_height, timeout_ms);
    }

    auto duplication = static_cast<IDXGIOutputDuplication*>(duplication_);
    auto context = static_cast<ID3D11DeviceContext*>(context_);

    if (!duplication || !context) {
        return Result::Error;
    }

    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    IDXGIResource* desktop_resource = nullptr;

    HRESULT hr = duplication->AcquireNextFrame(
        static_cast<UINT>(timeout_ms),
        &frame_info,
        &desktop_resource
    );

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return Result::Idle;
    }

    if (hr == DXGI_ERROR_ACCESS_LOST) {
        return Result::Lost;
    }

    if (FAILED(hr)) {
        return Result::Lost;
    }

    // LastPresentTime == 0 means only the mouse pointer moved; the desktop
    // image itself is unchanged, so there is nothing new to encode.
    if (frame_info.LastPresentTime.QuadPart == 0 && delivered_first_) {
        release_com(desktop_resource);
        duplication->ReleaseFrame();
        return Result::Idle;
    }

    ID3D11Texture2D* frame_texture = nullptr;
    hr = desktop_resource->QueryInterface(IID_PPV_ARGS(&frame_texture));
    release_com(desktop_resource);

    if (FAILED(hr)) {
        duplication->ReleaseFrame();
        return Result::Lost;
    }

    D3D11_TEXTURE2D_DESC tex_desc{};
    frame_texture->GetDesc(&tex_desc);

    if (staging_ == nullptr ||
        staging_width_ != tex_desc.Width ||
        staging_height_ != tex_desc.Height ||
        staging_format_ != static_cast<int>(tex_desc.Format)) {

        if (!create_staging(tex_desc.Width, tex_desc.Height, static_cast<int>(tex_desc.Format))) {
            release_com(frame_texture);
            duplication->ReleaseFrame();
            return Result::Error;
        }
    }

    auto staging = static_cast<ID3D11Texture2D*>(staging_);
    context->CopyResource(staging, frame_texture);
    release_com(frame_texture);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);

    if (FAILED(hr)) {
        duplication->ReleaseFrame();
        return Result::Lost;
    }

    // Crop to even dimensions (NV12 requirement) and tightly pack.
    int even_width = static_cast<int>(tex_desc.Width) & ~1;
    int even_height = static_cast<int>(tex_desc.Height) & ~1;

    out_width = even_width;
    out_height = even_height;
    out_bgra.resize(static_cast<size_t>(even_width) * even_height * 4);

    const unsigned char* src = static_cast<const unsigned char*>(mapped.pData);
    unsigned char* dst = out_bgra.data();
    size_t row_bytes = static_cast<size_t>(even_width) * 4;

    for (int y = 0; y < even_height; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * row_bytes,
                    src + static_cast<size_t>(y) * mapped.RowPitch,
                    row_bytes);
    }

    context->Unmap(staging, 0);
    duplication->ReleaseFrame();

    delivered_first_ = true;
    return Result::Frame;
}

bool ScreenDuplicator::init_gdi() {
    int screen_width = GetSystemMetrics(SM_CXSCREEN);
    int screen_height = GetSystemMetrics(SM_CYSCREEN);

    if (screen_width <= 0 || screen_height <= 0) {
        return false;
    }

    desktop_width_ = screen_width;
    desktop_height_ = screen_height;
    use_gdi_ = true;
    delivered_first_ = false;
    prev_frame_.clear();
    last_tick_ = 0;
    return true;
}

ScreenDuplicator::Result ScreenDuplicator::acquire_gdi(
    std::vector<unsigned char>& out_bgra,
    int& out_width,
    int& out_height,
    int timeout_ms
) {
    // Pace to roughly one frame per timeout_ms; GDI has no blocking wait.
    unsigned long long now = GetTickCount64();
    if (last_tick_ != 0) {
        unsigned long long elapsed = now - last_tick_;
        if (timeout_ms > 0 && elapsed < static_cast<unsigned long long>(timeout_ms)) {
            Sleep(static_cast<DWORD>(timeout_ms - elapsed));
        }
    }
    last_tick_ = GetTickCount64();

    int screen_width = GetSystemMetrics(SM_CXSCREEN);
    int screen_height = GetSystemMetrics(SM_CYSCREEN);

    if (screen_width <= 0 || screen_height <= 0) {
        return Result::Error;
    }

    desktop_width_ = screen_width;
    desktop_height_ = screen_height;

    int even_width = screen_width & ~1;
    int even_height = screen_height & ~1;

    HDC screen_dc = GetDC(nullptr);
    if (!screen_dc) {
        return Result::Error;
    }

    HDC memory_dc = CreateCompatibleDC(screen_dc);
    if (!memory_dc) {
        ReleaseDC(nullptr, screen_dc);
        return Result::Error;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = even_width;
    bmi.bmiHeader.biHeight = -even_height; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* raw_bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(memory_dc, &bmi, DIB_RGB_COLORS, &raw_bits, nullptr, 0);

    if (!bitmap || !raw_bits) {
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        return Result::Error;
    }

    HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);

    BOOL ok = BitBlt(memory_dc, 0, 0, even_width, even_height, screen_dc, 0, 0, SRCCOPY);

    Result result = Result::Error;

    if (ok) {
        size_t data_size = static_cast<size_t>(even_width) * even_height * 4;

        bool unchanged =
            delivered_first_ &&
            prev_frame_.size() == data_size &&
            std::memcmp(prev_frame_.data(), raw_bits, data_size) == 0;

        if (unchanged) {
            result = Result::Idle;
        } else {
            out_width = even_width;
            out_height = even_height;
            out_bgra.resize(data_size);
            std::memcpy(out_bgra.data(), raw_bits, data_size);

            prev_frame_.assign(out_bgra.begin(), out_bgra.end());
            delivered_first_ = true;
            result = Result::Frame;
        }
    }

    SelectObject(memory_dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(nullptr, screen_dc);

    return result;
}

}
