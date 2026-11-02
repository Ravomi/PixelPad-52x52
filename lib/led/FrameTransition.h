#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace pixelpad {
// Display-worker-owned, fixed-size source frame. Targets remain in PublishedFrame.
template<size_t Bytes> class FrameTransition {
public:
    void begin(const uint8_t* displayed, uint32_t now, uint16_t durationMs) {
        duration_ = durationMs > 500 ? 500 : durationMs;
        started_ = now;
        active_ = duration_ != 0;
        if (active_) memcpy(from_, displayed, Bytes);
    }
    bool active() const { return active_; }
    void apply(uint8_t* target, uint32_t now) {
        if (!active_) return;
        const uint32_t elapsed = now - started_;
        if (elapsed >= duration_) { active_ = false; return; }
        // Smoothstep, fixed point 0..256. No allocations or floating point.
        const uint32_t t = elapsed * 256u / duration_;
        const uint32_t alpha = (t * t * (768u - 2u * t) + 32768u) >> 16;
        for (size_t i = 0; i < Bytes; ++i)
            target[i] = (from_[i] * (256u-alpha) + target[i] * alpha + 128u) >> 8;
    }
private:
    uint8_t from_[Bytes]{};
    uint32_t started_ = 0;
    uint16_t duration_ = 0;
    bool active_ = false;
};
}
