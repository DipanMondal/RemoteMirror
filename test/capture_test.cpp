// Quick standalone check that DXGI Desktop Duplication initializes and yields
// frames on this machine. Prints the HRESULT on failure so hybrid-GPU / codec
// issues are diagnosable.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <cstdio>
#include <vector>

#include "capture.h"

int main() {
    rm::ScreenDuplicator dup;

    if (!dup.initialize()) {
        std::printf("init FAILED  hr=0x%08lX\n", static_cast<unsigned long>(dup.last_error()));
        return 1;
    }

    std::printf("init OK  desktop=%dx%d\n", dup.desktop_width(), dup.desktop_height());

    int frames = 0;
    int idle = 0;

    for (int i = 0; i < 80 && frames < 3; ++i) {
        std::vector<unsigned char> bgra;
        int w = 0;
        int h = 0;

        rm::ScreenDuplicator::Result r = dup.acquire(bgra, w, h, 100);

        if (r == rm::ScreenDuplicator::Result::Frame) {
            ++frames;
            std::printf("frame %d: %dx%d  bytes=%zu\n", frames, w, h, bgra.size());
        } else if (r == rm::ScreenDuplicator::Result::Idle) {
            ++idle;
        } else if (r == rm::ScreenDuplicator::Result::Lost) {
            std::printf("acquire: Lost (reinit needed)\n");
        } else {
            std::printf("acquire: Error\n");
            break;
        }
    }

    std::printf("frames=%d idle=%d  %s\n", frames, idle, frames > 0 ? "PASS" : "FAIL");
    return frames > 0 ? 0 : 1;
}
