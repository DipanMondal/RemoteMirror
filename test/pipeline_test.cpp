// Loopback validation for the H.264 encode -> decode pipeline.
//
// Runs entirely on one machine (no sockets, no GUI). Generates synthetic BGRA
// frames with known colours, encodes them to Annex-B H.264, feeds the units
// straight into the decoder, and checks that frames come back with roughly the
// right colours. This validates the riskiest parts: bitstream compatibility
// (SPS/PPS + start codes) and the BT.601 NV12 conversions on both sides.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <mfapi.h>

#include <cstdio>
#include <vector>

#include "encoder.h"
#include "decoder.h"

namespace {

const int kWidth = 320;
const int kHeight = 240;
const int kFrames = 12;

// Top half red, bottom half blue (BGRA).
void make_frame(std::vector<unsigned char>& bgra, int shift) {
    bgra.resize(static_cast<size_t>(kWidth) * kHeight * 4);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            unsigned char* p = bgra.data() + (static_cast<size_t>(y) * kWidth + x) * 4;
            bool top = y < kHeight / 2;
            // small horizontal shift each frame so there is real motion to encode
            int xx = (x + shift) % kWidth;
            unsigned char ramp = static_cast<unsigned char>(xx * 255 / kWidth);
            if (top) {
                p[0] = 0;          // B
                p[1] = 0;          // G
                p[2] = 255;        // R
            } else {
                p[0] = 255;        // B
                p[1] = 0;          // G
                p[2] = 0;          // R
            }
            p[3] = ramp;           // A (ignored)
        }
    }
}

void sample(const rm::DecodedFrame& f, int x, int y, int& b, int& g, int& r) {
    const unsigned char* p = f.bgra.data() + (static_cast<size_t>(y) * f.width + x) * 4;
    b = p[0];
    g = p[1];
    r = p[2];
}

} // namespace

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        std::printf("CoInitializeEx failed\n");
        return 2;
    }
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        std::printf("MFStartup failed\n");
        return 2;
    }

    int rc = 1;

    do {
        rm::H264Encoder encoder;
        if (!encoder.initialize(kWidth, kHeight, 30, 1500000)) {
            std::printf("FAIL: encoder.initialize\n");
            break;
        }

        rm::H264Decoder decoder;
        if (!decoder.initialize()) {
            std::printf("FAIL: decoder.initialize\n");
            break;
        }

        int total_units = 0;
        int total_keyframes = 0;
        int total_decoded = 0;
        rm::DecodedFrame last_decoded;

        std::vector<unsigned char> frame;

        for (int i = 0; i < kFrames; ++i) {
            make_frame(frame, i * 8);

            std::vector<std::vector<unsigned char>> units;
            std::vector<bool> keys;

            if (!encoder.encode(frame.data(), i == 0, units, keys)) {
                std::printf("FAIL: encoder.encode at frame %d\n", i);
                break;
            }

            for (size_t u = 0; u < units.size(); ++u) {
                ++total_units;
                if (keys[u]) {
                    ++total_keyframes;
                }

                std::vector<rm::DecodedFrame> out;
                if (!decoder.decode(units[u].data(), units[u].size(), out)) {
                    std::printf("FAIL: decoder.decode (unit %d)\n", total_units);
                    break;
                }

                for (auto& d : out) {
                    ++total_decoded;
                    last_decoded = d;
                }
            }
        }

        std::printf("units=%d keyframes=%d decoded_frames=%d\n",
                    total_units, total_keyframes, total_decoded);

        if (total_units == 0) {
            std::printf("FAIL: encoder produced no access units\n");
            break;
        }
        if (total_keyframes == 0) {
            std::printf("FAIL: no keyframe was produced/flagged\n");
            break;
        }
        if (total_decoded == 0) {
            std::printf("FAIL: decoder produced no frames (bitstream incompatible)\n");
            break;
        }
        if (last_decoded.width != kWidth || last_decoded.height != kHeight) {
            std::printf("FAIL: decoded size %dx%d != %dx%d\n",
                        last_decoded.width, last_decoded.height, kWidth, kHeight);
            break;
        }

        // Colour check (generous tolerance for the YUV 4:2:0 round trip).
        int tb, tg, tr, bb, bg, br;
        sample(last_decoded, kWidth / 2, kHeight / 4, tb, tg, tr);       // top -> red
        sample(last_decoded, kWidth / 2, kHeight * 3 / 4, bb, bg, br);   // bottom -> blue

        std::printf("top  pixel BGR=(%d,%d,%d) expect ~red\n", tb, tg, tr);
        std::printf("bot  pixel BGR=(%d,%d,%d) expect ~blue\n", bb, bg, br);

        bool red_ok = tr > 150 && tb < 110 && tg < 110;
        bool blue_ok = bb > 150 && br < 110 && bg < 110;

        if (!red_ok || !blue_ok) {
            std::printf("FAIL: colour round-trip out of tolerance\n");
            break;
        }

        std::printf("PASS: encode/decode pipeline + colour conversion OK\n");
        rc = 0;
    } while (false);

    MFShutdown();
    CoUninitialize();
    return rc;
}
