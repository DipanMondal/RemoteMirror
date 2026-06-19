#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "decoder.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>

#include <algorithm>
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

unsigned char clamp_byte(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<unsigned char>(value);
}

} // namespace

H264Decoder::~H264Decoder() {
    shutdown();
}

void H264Decoder::shutdown() {
    auto transform = static_cast<IMFTransform*>(transform_);

    if (transform) {
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }

    release_com(transform);
    transform_ = nullptr;

    width_ = 0;
    height_ = 0;
    stride_ = 0;
    output_ready_ = false;
    provides_samples_ = false;
    output_size_ = 0;
    pts_ = 0;
}

bool H264Decoder::initialize() {
    shutdown();

    IMFTransform* transform = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_CMSH264DecoderMFT,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&transform)
    );

    if (FAILED(hr)) {
        return false;
    }

    transform_ = transform;

    IMFAttributes* attributes = nullptr;
    if (SUCCEEDED(transform->GetAttributes(&attributes)) && attributes) {
        attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        attributes->Release();
    }

    IMFMediaType* in_type = nullptr;
    if (FAILED(MFCreateMediaType(&in_type))) {
        shutdown();
        return false;
    }

    bool ok = false;
    do {
        if (FAILED(in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))) break;
        if (FAILED(in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264))) break;
        if (FAILED(in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive))) break;
        if (FAILED(transform->SetInputType(0, in_type, 0))) break;
        ok = true;
    } while (false);

    release_com(in_type);

    if (!ok) {
        shutdown();
        return false;
    }

    transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool H264Decoder::negotiate_output_type() {
    auto transform = static_cast<IMFTransform*>(transform_);

    for (DWORD i = 0;; ++i) {
        IMFMediaType* candidate = nullptr;
        HRESULT hr = transform->GetOutputAvailableType(0, i, &candidate);

        if (hr == MF_E_NO_MORE_TYPES) {
            return false;
        }
        if (FAILED(hr) || !candidate) {
            return false;
        }

        GUID subtype{};
        candidate->GetGUID(MF_MT_SUBTYPE, &subtype);

        if (subtype == MFVideoFormat_NV12) {
            hr = transform->SetOutputType(0, candidate, 0);
            if (SUCCEEDED(hr)) {
                UINT32 w = 0;
                UINT32 h = 0;
                MFGetAttributeSize(candidate, MF_MT_FRAME_SIZE, &w, &h);
                width_ = static_cast<int>(w);
                height_ = static_cast<int>(h);

                UINT32 stride = 0;
                if (SUCCEEDED(candidate->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) && stride > 0) {
                    stride_ = static_cast<int>(stride);
                } else {
                    stride_ = width_;
                }

                MFT_OUTPUT_STREAM_INFO stream_info{};
                transform->GetOutputStreamInfo(0, &stream_info);
                provides_samples_ =
                    (stream_info.dwFlags &
                     (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
                output_size_ = stream_info.cbSize;

                output_ready_ = true;
                candidate->Release();
                return true;
            }
        }

        candidate->Release();
    }
}

void H264Decoder::nv12_to_bgra(const unsigned char* nv12, int stride, DecodedFrame& frame) {
    const int w = width_;
    const int h = height_;

    if (stride <= 0) {
        stride = w;
    }

    const unsigned char* y_plane = nv12;
    const unsigned char* uv_plane = nv12 + static_cast<size_t>(stride) * h;

    frame.width = w;
    frame.height = h;
    frame.bgra.resize(static_cast<size_t>(w) * h * 4);
    unsigned char* dst = frame.bgra.data();

    // BT.601 studio-swing inverse (paired with the encoder).
    for (int y = 0; y < h; ++y) {
        const unsigned char* y_row = y_plane + static_cast<size_t>(y) * stride;
        const unsigned char* uv_row = uv_plane + static_cast<size_t>(y / 2) * stride;
        unsigned char* out_row = dst + static_cast<size_t>(y) * w * 4;

        for (int x = 0; x < w; ++x) {
            int c = y_row[x] - 16;
            int d = uv_row[(x & ~1) + 0] - 128;
            int e = uv_row[(x & ~1) + 1] - 128;

            int r = (298 * c + 409 * e + 128) >> 8;
            int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int b = (298 * c + 516 * d + 128) >> 8;

            out_row[x * 4 + 0] = clamp_byte(b);
            out_row[x * 4 + 1] = clamp_byte(g);
            out_row[x * 4 + 2] = clamp_byte(r);
            out_row[x * 4 + 3] = 255;
        }
    }
}

bool H264Decoder::drain_output(std::vector<DecodedFrame>& out_frames) {
    auto transform = static_cast<IMFTransform*>(transform_);

    for (;;) {
        IMFSample* output_sample = nullptr;

        if (output_ready_ && !provides_samples_) {
            IMFMediaBuffer* buffer = nullptr;
            if (FAILED(MFCreateSample(&output_sample))) {
                return false;
            }
            if (FAILED(MFCreateMemoryBuffer(std::max<DWORD>(output_size_, 1u), &buffer))) {
                release_com(output_sample);
                return false;
            }
            output_sample->AddBuffer(buffer);
            buffer->Release();
        }

        MFT_OUTPUT_DATA_BUFFER output_data{};
        output_data.dwStreamID = 0;
        output_data.pSample = output_sample;
        output_data.dwStatus = 0;
        output_data.pEvents = nullptr;

        DWORD status = 0;
        HRESULT hr = transform->ProcessOutput(0, 1, &output_data, &status);

        if (output_data.pEvents) {
            output_data.pEvents->Release();
            output_data.pEvents = nullptr;
        }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            release_com(output_sample);
            return true;
        }

        // The MS H.264 decoder reports TYPE_NOT_SET (before any output type is
        // set) or STREAM_CHANGE (on a resolution change) to ask us to (re)pick
        // the output format. Both are handled by negotiating NV12.
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE || hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
            release_com(output_sample);
            if (!negotiate_output_type()) {
                // Decoder hasn't parsed enough of the stream yet; wait for more.
                return true;
            }
            continue;
        }

        if (FAILED(hr)) {
            release_com(output_sample);
            return false;
        }

        IMFSample* produced = output_data.pSample;
        if (!produced) {
            continue;
        }

        IMFMediaBuffer* contiguous = nullptr;
        if (SUCCEEDED(produced->ConvertToContiguousBuffer(&contiguous)) && contiguous) {
            BYTE* data = nullptr;
            DWORD current_length = 0;
            if (SUCCEEDED(contiguous->Lock(&data, nullptr, &current_length))) {
                if (width_ > 0 && height_ > 0) {
                    DecodedFrame frame;
                    nv12_to_bgra(data, stride_, frame);
                    out_frames.push_back(std::move(frame));
                }
                contiguous->Unlock();
            }
            contiguous->Release();
        }

        produced->Release();
    }
}

bool H264Decoder::decode(const unsigned char* data, size_t size, std::vector<DecodedFrame>& out_frames) {
    auto transform = static_cast<IMFTransform*>(transform_);

    if (!transform || !data || size == 0) {
        return false;
    }

    IMFSample* input_sample = nullptr;
    IMFMediaBuffer* input_buffer = nullptr;

    if (FAILED(MFCreateSample(&input_sample))) {
        return false;
    }

    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(size), &input_buffer))) {
        release_com(input_sample);
        return false;
    }

    BYTE* dst = nullptr;
    if (FAILED(input_buffer->Lock(&dst, nullptr, nullptr))) {
        release_com(input_buffer);
        release_com(input_sample);
        return false;
    }

    std::memcpy(dst, data, size);
    input_buffer->Unlock();
    input_buffer->SetCurrentLength(static_cast<DWORD>(size));

    input_sample->AddBuffer(input_buffer);
    input_sample->SetSampleTime(pts_);
    input_sample->SetSampleDuration(frame_duration_);
    pts_ += frame_duration_;

    // Submit the sample, draining pending output if the decoder is full.
    bool ok = true;
    HRESULT hr = transform->ProcessInput(0, input_sample, 0);

    if (hr == MF_E_NOTACCEPTING) {
        ok = drain_output(out_frames);
        if (ok) {
            hr = transform->ProcessInput(0, input_sample, 0);
        }
    }

    release_com(input_buffer);
    release_com(input_sample);

    if (!ok || FAILED(hr)) {
        return false;
    }

    return drain_output(out_frames);
}

}
