#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "encoder.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
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

H264Encoder::~H264Encoder() {
    shutdown();
}

void H264Encoder::shutdown() {
    auto transform = static_cast<IMFTransform*>(transform_);
    auto codec_api = static_cast<ICodecAPI*>(codec_api_);

    if (transform) {
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }

    release_com(codec_api);
    release_com(transform);

    transform_ = nullptr;
    codec_api_ = nullptr;

    width_ = 0;
    height_ = 0;
    pts_ = 0;
    provides_samples_ = false;
    output_size_ = 0;
    nv12_.clear();
    sequence_header_.clear();
}

bool H264Encoder::initialize(int width, int height, int fps, int bitrate_bps) {
    shutdown();

    if (width <= 0 || height <= 0 || (width & 1) || (height & 1)) {
        return false;
    }

    width_ = width;
    height_ = height;
    fps_ = (fps > 0) ? fps : 30;
    bitrate_ = (bitrate_bps > 0) ? bitrate_bps : 4000000;
    pts_ = 0;
    frame_duration_ = 10000000LL / fps_; // 100ns units

    IMFTransform* transform = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_CMSH264EncoderMFT,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&transform)
    );

    if (FAILED(hr)) {
        return false;
    }

    transform_ = transform;

    // Best-effort low-latency hint on the transform itself.
    IMFAttributes* attributes = nullptr;
    if (SUCCEEDED(transform->GetAttributes(&attributes)) && attributes) {
        attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        attributes->Release();
    }

    if (!configure_types()) {
        shutdown();
        return false;
    }

    // Grab ICodecAPI for bitrate / latency / GOP control before streaming.
    ICodecAPI* codec_api = nullptr;
    if (SUCCEEDED(transform->QueryInterface(IID_PPV_ARGS(&codec_api)))) {
        codec_api_ = codec_api;
        apply_codec_settings();
    }

    MFT_OUTPUT_STREAM_INFO stream_info{};
    if (FAILED(transform->GetOutputStreamInfo(0, &stream_info))) {
        shutdown();
        return false;
    }

    provides_samples_ =
        (stream_info.dwFlags &
         (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    output_size_ = stream_info.cbSize;

    transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    capture_sequence_header();

    nv12_.resize(static_cast<size_t>(width_) * height_ * 3 / 2);
    return true;
}

bool H264Encoder::configure_types() {
    auto transform = static_cast<IMFTransform*>(transform_);

    // Output type (H.264) must be set before the input type on encoders.
    IMFMediaType* out_type = nullptr;
    if (FAILED(MFCreateMediaType(&out_type))) {
        return false;
    }

    bool ok = false;
    do {
        if (FAILED(out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))) break;
        if (FAILED(out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264))) break;
        if (FAILED(out_type->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrate_)))) break;
        if (FAILED(MFSetAttributeSize(out_type, MF_MT_FRAME_SIZE, width_, height_))) break;
        if (FAILED(MFSetAttributeRatio(out_type, MF_MT_FRAME_RATE, fps_, 1))) break;
        if (FAILED(out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive))) break;
        if (FAILED(MFSetAttributeRatio(out_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1))) break;
        if (FAILED(out_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main))) break;

        if (FAILED(transform->SetOutputType(0, out_type, 0))) break;
        ok = true;
    } while (false);

    release_com(out_type);
    if (!ok) {
        return false;
    }

    // Input type (NV12).
    IMFMediaType* in_type = nullptr;
    if (FAILED(MFCreateMediaType(&in_type))) {
        return false;
    }

    ok = false;
    do {
        if (FAILED(in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))) break;
        if (FAILED(in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12))) break;
        if (FAILED(MFSetAttributeSize(in_type, MF_MT_FRAME_SIZE, width_, height_))) break;
        if (FAILED(MFSetAttributeRatio(in_type, MF_MT_FRAME_RATE, fps_, 1))) break;
        if (FAILED(in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive))) break;
        if (FAILED(MFSetAttributeRatio(in_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1))) break;

        if (FAILED(transform->SetInputType(0, in_type, 0))) break;
        ok = true;
    } while (false);

    release_com(in_type);
    return ok;
}

void H264Encoder::apply_codec_settings() {
    auto codec_api = static_cast<ICodecAPI*>(codec_api_);

    auto set_u32 = [&](const GUID& key, UINT32 value) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = value;
        codec_api->SetValue(&key, &v);
        VariantClear(&v);
    };

    auto set_bool = [&](const GUID& key, bool value) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_BOOL;
        v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
        codec_api->SetValue(&key, &v);
        VariantClear(&v);
    };

    // All best-effort; encoders ignore keys they do not support.
    set_bool(CODECAPI_AVLowLatencyMode, true);
    set_u32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
    set_u32(CODECAPI_AVEncCommonMeanBitRate, static_cast<UINT32>(bitrate_));
    // Long GOP; we still force IDRs periodically for recovery / late joiners.
    set_u32(CODECAPI_AVEncMPVGOPSize, static_cast<UINT32>(fps_ * 2));
}

void H264Encoder::capture_sequence_header() {
    auto transform = static_cast<IMFTransform*>(transform_);

    IMFMediaType* current = nullptr;
    if (FAILED(transform->GetOutputCurrentType(0, &current)) || !current) {
        return;
    }

    UINT32 blob_size = 0;
    if (SUCCEEDED(current->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &blob_size)) && blob_size > 0) {
        sequence_header_.resize(blob_size);
        if (FAILED(current->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, sequence_header_.data(), blob_size, nullptr))) {
            sequence_header_.clear();
        }
    }

    current->Release();
}

void H264Encoder::bgra_to_nv12(const unsigned char* bgra) {
    const int w = width_;
    const int h = height_;

    unsigned char* y_plane = nv12_.data();
    unsigned char* uv_plane = nv12_.data() + static_cast<size_t>(w) * h;

    // BT.601 studio-swing. Must stay paired with the decoder's inverse.
    for (int y = 0; y < h; ++y) {
        const unsigned char* row = bgra + static_cast<size_t>(y) * w * 4;
        unsigned char* y_row = y_plane + static_cast<size_t>(y) * w;

        for (int x = 0; x < w; ++x) {
            int b = row[x * 4 + 0];
            int g = row[x * 4 + 1];
            int r = row[x * 4 + 2];

            int luma = (66 * r + 129 * g + 25 * b + 128);
            y_row[x] = clamp_byte((luma >> 8) + 16);
        }
    }

    // Chroma: average each 2x2 block for better quality.
    for (int y = 0; y < h; y += 2) {
        unsigned char* uv_row = uv_plane + static_cast<size_t>(y / 2) * w;

        for (int x = 0; x < w; x += 2) {
            const unsigned char* p00 = bgra + (static_cast<size_t>(y) * w + x) * 4;
            const unsigned char* p01 = p00 + 4;
            const unsigned char* p10 = p00 + static_cast<size_t>(w) * 4;
            const unsigned char* p11 = p10 + 4;

            int b = (p00[0] + p01[0] + p10[0] + p11[0] + 2) >> 2;
            int g = (p00[1] + p01[1] + p10[1] + p11[1] + 2) >> 2;
            int r = (p00[2] + p01[2] + p10[2] + p11[2] + 2) >> 2;

            int u = (-38 * r - 74 * g + 112 * b + 128);
            int v = (112 * r - 94 * g - 18 * b + 128);

            uv_row[x + 0] = clamp_byte((u >> 8) + 128);
            uv_row[x + 1] = clamp_byte((v >> 8) + 128);
        }
    }
}

bool H264Encoder::drain_output(
    std::vector<std::vector<unsigned char>>& out_units,
    std::vector<bool>& out_keyframe
) {
    auto transform = static_cast<IMFTransform*>(transform_);

    for (;;) {
        IMFSample* output_sample = nullptr;

        if (!provides_samples_) {
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

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // Output format renegotiation; refresh the sequence header and retry.
            release_com(output_sample);
            capture_sequence_header();
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

        if (sequence_header_.empty()) {
            capture_sequence_header();
        }

        UINT32 clean_point = MFGetAttributeUINT32(produced, MFSampleExtension_CleanPoint, 0);
        bool keyframe = clean_point != 0;

        IMFMediaBuffer* contiguous = nullptr;
        if (SUCCEEDED(produced->ConvertToContiguousBuffer(&contiguous)) && contiguous) {
            BYTE* data = nullptr;
            DWORD current_length = 0;
            if (SUCCEEDED(contiguous->Lock(&data, nullptr, &current_length))) {
                std::vector<unsigned char> unit;

                // Prepend SPS/PPS to keyframes so late joiners / recovery work.
                if (keyframe && !sequence_header_.empty()) {
                    unit.reserve(sequence_header_.size() + current_length);
                    unit.insert(unit.end(), sequence_header_.begin(), sequence_header_.end());
                }

                unit.insert(unit.end(), data, data + current_length);
                contiguous->Unlock();

                if (!unit.empty()) {
                    out_units.push_back(std::move(unit));
                    out_keyframe.push_back(keyframe);
                }
            }
            contiguous->Release();
        }

        produced->Release();
    }
}

bool H264Encoder::encode(
    const unsigned char* bgra,
    bool force_keyframe,
    std::vector<std::vector<unsigned char>>& out_units,
    std::vector<bool>& out_keyframe
) {
    auto transform = static_cast<IMFTransform*>(transform_);
    auto codec_api = static_cast<ICodecAPI*>(codec_api_);

    if (!transform || !bgra) {
        return false;
    }

    if (force_keyframe && codec_api) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = 1;
        codec_api->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
        VariantClear(&v);
    }

    bgra_to_nv12(bgra);

    IMFSample* input_sample = nullptr;
    IMFMediaBuffer* input_buffer = nullptr;

    DWORD nv12_size = static_cast<DWORD>(nv12_.size());

    if (FAILED(MFCreateSample(&input_sample))) {
        return false;
    }

    if (FAILED(MFCreateMemoryBuffer(nv12_size, &input_buffer))) {
        release_com(input_sample);
        return false;
    }

    BYTE* dst = nullptr;
    if (FAILED(input_buffer->Lock(&dst, nullptr, nullptr))) {
        release_com(input_buffer);
        release_com(input_sample);
        return false;
    }

    std::memcpy(dst, nv12_.data(), nv12_size);
    input_buffer->Unlock();
    input_buffer->SetCurrentLength(nv12_size);

    input_sample->AddBuffer(input_buffer);
    input_sample->SetSampleTime(pts_);
    input_sample->SetSampleDuration(frame_duration_);
    pts_ += frame_duration_;

    HRESULT hr = transform->ProcessInput(0, input_sample, 0);

    release_com(input_buffer);
    release_com(input_sample);

    if (hr == MF_E_NOTACCEPTING) {
        // Drain pending output then retry once.
        if (!drain_output(out_units, out_keyframe)) {
            return false;
        }
        return drain_output(out_units, out_keyframe);
    }

    if (FAILED(hr)) {
        return false;
    }

    return drain_output(out_units, out_keyframe);
}

}
