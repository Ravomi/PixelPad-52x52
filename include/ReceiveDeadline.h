#pragma once
#include <stdint.h>

namespace pixelpad {
// A slow but progressing transfer is different from a stalled one. Unsigned
// elapsed times also work when millis() wraps during an in-flight image.
class ReceiveDeadline {
public:
    ReceiveDeadline(uint32_t now, uint32_t idleMs, uint32_t totalMs)
        : started_(now), progress_(now), idleMs_(idleMs), totalMs_(totalMs) {}
    void madeProgress(uint32_t now) { progress_ = now; }
    uint32_t remainingMs(uint32_t now) const {
        const uint32_t idle = now - progress_, total = now - started_;
        if (idle >= idleMs_ || total >= totalMs_) return 0;
        const uint32_t idleLeft = idleMs_ - idle, totalLeft = totalMs_ - total;
        return idleLeft < totalLeft ? idleLeft : totalLeft;
    }
private:
    uint32_t started_, progress_, idleMs_, totalMs_;
};
}
