#ifndef PIXELPAD_BRIGHTNESS_POLICY_H
#define PIXELPAD_BRIGHTNESS_POLICY_H

#include <stdint.h>

namespace pixelpad {
// App and knob retain their full logical range. Apply the 90% hardware limit
// once, at the DMA driver boundary. Floor rounding never exceeds the limit.
constexpr uint8_t kBrightnessPercent = 90;
constexpr uint8_t scaleBrightness(uint8_t requested) {
    return static_cast<uint8_t>(static_cast<uint16_t>(requested) *
                                kBrightnessPercent / 100);
}
}  // namespace pixelpad

#endif
