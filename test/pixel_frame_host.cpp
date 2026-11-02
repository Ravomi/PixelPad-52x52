#include "PixelFrame.h"
#include <array>
#include <cassert>
#include <cstring>
#include <random>

using pixelpad::PixelFrame;
using Image = std::array<PixelFrame::RGB, PixelFrame::kPixels>;

int main() {
    PixelFrame frame;
    Image expected{}, buffers[2]{};
    unsigned calls = 0;
    auto flush = [&](unsigned back) {
        calls = 0;
        const unsigned writes = frame.flush(back, [&](unsigned x, unsigned y, uint8_t r, uint8_t g, uint8_t b) {
            assert(x < 52 && y < 52);
            buffers[back][y * 52 + x] = {r, g, b};
            ++calls;
        });
        assert(writes == calls);
        assert(std::memcmp(buffers[back].data(), expected.data(), sizeof(expected)) == 0);
        assert(!frame.changed());
    };
    frame.set(-1, 0, 1, 2, 3);
    frame.set(0, -1, 1, 2, 3);
    frame.set(52, 0, 1, 2, 3);
    frame.set(0, 52, 1, 2, 3);
    frame.clear();
    assert(!frame.changed());

    frame.set(0, 0, 255, 0, 0);
    expected[0] = {255, 0, 0};
    flush(1);
    assert(calls == 1);
    frame.set(51, 51, 0, 255, 0);
    expected.back() = {0, 255, 0};
    flush(0); // This buffer also needs the previous frame's red pixel.
    assert(calls == 2);
    frame.set(0, 0, 0, 0, 0);
    expected[0] = {0, 0, 0};
    flush(1); // Erasure persists; the other frame's green patch is retained.
    assert(calls == 2);

    // Compare alternating DMA images against a full reference across clears,
    // patches, overwrites within one frame and the partially occupied last word.
    std::mt19937 random(52);
    for (unsigned iteration = 0; iteration < 500; ++iteration) {
        if (iteration % 13 == 0) {
            frame.clear();
            expected.fill({0, 0, 0});
        }
        for (unsigned j = 0; j < 40; ++j) {
            const unsigned index = random() % expected.size();
            PixelFrame::RGB rgb{uint8_t(random()), uint8_t(random()), uint8_t(random())};
            frame.set(index % 52, index / 52, rgb.r, rgb.g, rgb.b);
            expected[index] = rgb;
        }
        flush(iteration % 2);
    }
    frame.clear();
    expected.fill({0, 0, 0});
    flush(0);
    flush(1);
    flush(0);
    assert(calls == 0); // No writes for an unchanged frame.

    using pixelpad::dmaBufferReusable;
    assert(dmaBufferReusable(0, 0, 0, false));
    assert(!dmaBufferReusable(100, 0, 0, true));
    assert(!dmaBufferReusable(1961, 0, 1075, true));
    assert(dmaBufferReusable(1962, 0, 1075, true));
    assert(!dmaBufferReusable(3077, 0, 672, true));
    assert(dmaBufferReusable(3078, 0, 672, true));
    assert(!dmaBufferReusable(6701, 0, 303, true));
    assert(dmaBufferReusable(6702, 0, 303, true));
    const uint32_t flipped = UINT32_MAX - 1000;
    assert(!dmaBufferReusable(flipped + 1961u, flipped, 1075, true));
    assert(dmaBufferReusable(flipped + 1962u, flipped, 1075, true));
}
