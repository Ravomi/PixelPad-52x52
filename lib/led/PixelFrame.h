#pragma once

#include <stdint.h>

namespace pixelpad {

// RGB is staged independently of DMA. Each DMA buffer keeps its own dirty map,
// so a patch on the next frame cannot resurrect pixels from two frames ago.
// Only the render task may access this object after setup.
class PixelFrame {
public:
    static constexpr unsigned kWidth = 52, kHeight = 52;
    static constexpr unsigned kPixels = kWidth * kHeight;
    struct RGB { uint8_t r, g, b; };

    void set(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || y < 0 || x >= int(kWidth) || y >= int(kHeight)) return;
        const unsigned index = y * kWidth + x;
        RGB& old = pixels_[index];
        if (old.r == r && old.g == g && old.b == b) return;
        old = {r, g, b};
        dirty_[0][index / 32] |= uint32_t(1) << (index % 32);
        dirty_[1][index / 32] |= uint32_t(1) << (index % 32);
        changed_ = true;
    }

    void clear() {
        for (unsigned y = 0; y < kHeight; ++y)
            for (unsigned x = 0; x < kWidth; ++x) set(x, y, 0, 0, 0);
    }

    bool changed() const { return changed_; }

    // Caller must ensure this DMA buffer is no longer being scanned. This
    // writes the final RGB of each changed pixel once, including after clear().
    template <typename Draw>
    unsigned flush(unsigned buffer, Draw draw) {
        unsigned writes = 0;
        for (unsigned word = 0; word < kWords; ++word) {
            uint32_t bits = dirty_[buffer][word];
            while (bits) {
                const unsigned index = word * 32 + __builtin_ctz(bits);
                const RGB rgb = pixels_[index];
                draw(index % kWidth, index / kWidth, rgb.r, rgb.g, rgb.b);
                bits &= bits - 1;
                ++writes;
            }
            dirty_[buffer][word] = 0;
        }
        changed_ = false;
        return writes;
    }

private:
    static constexpr unsigned kWords = (kPixels + 31) / 32;
    RGB pixels_[kPixels]{};
    uint32_t dirty_[2][kWords]{};
    bool changed_ = false;
};

// The pinned S3 driver switches descriptor rings asynchronously. Keep the old
// front buffer untouched for two complete scans plus a FIFO/prefetch margin.
// Unsigned subtraction also works across the micros() wrap boundary.
inline bool dmaBufferReusable(uint32_t nowUs, uint32_t flippedUs,
                              uint32_t scanHz, bool hasFlipped) {
    if (!hasFlipped) return true;
    if (!scanHz) return false;
    const uint32_t guardUs = 2 * ((1000000UL + scanHz - 1) / scanHz) + 100;
    return uint32_t(nowUs - flippedUs) >= guardUs;
}
} // namespace pixelpad
