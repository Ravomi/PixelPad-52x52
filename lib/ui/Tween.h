#pragma once

#include <math.h>
#include <stdint.h>

namespace pixelpad {

class Tween {
public:
    enum class Easing : uint8_t { Linear, Smoothstep, EaseOut, Astra };

    // ForgeUI scene animations can use the same ease-out shape as AstraUI.
    static uint16_t progress(uint32_t elapsed, uint32_t duration, Easing easing) {
        if (duration == 0 || elapsed >= duration) return 65535u;
        const uint32_t t = elapsed * 65535u / duration;
        if (easing != Easing::Astra) return t;
        const uint32_t remaining = 65535u - t;
        const uint64_t squared = uint64_t(remaining) * remaining;
        return static_cast<uint16_t>(65535u - ((squared * squared) >> 48));
    }

    void start(int32_t from, int32_t to, uint32_t now, uint16_t duration,
               Easing easing = Easing::Smoothstep) {
        from_ = from;
        to_ = to;
        started_ = now;
        duration_ = duration;
        easing_ = easing;
        active_ = duration != 0 && from != to;
        if (!active_) value_ = to_;
    }

    int32_t update(uint32_t now) {
        if (!active_) return value_;
        const uint32_t elapsed = now - started_;
        if (elapsed >= duration_) {
            value_ = to_;
            active_ = false;
            return value_;
        }
        uint32_t eased = progress(elapsed, duration_, easing_);
        if (easing_ == Easing::Smoothstep) {
            const uint32_t t = elapsed * 65535u / duration_;
            eased = static_cast<uint32_t>((static_cast<uint64_t>(t) * t * (196605u - 2u * t) + 32768u) >> 32);
        } else if (easing_ == Easing::EaseOut) {
            const uint32_t t = elapsed * 65535u / duration_;
            const uint64_t remaining = 65535u - t;
            eased = 65535u - static_cast<uint32_t>((remaining * remaining) >> 16);
        }
        value_ = from_ + static_cast<int32_t>((static_cast<int64_t>(to_ - from_) * eased + 32767) / 65535);
        return value_;
    }

    bool active() const { return active_; }
    int32_t value() const { return value_; }

private:
    int32_t from_ = 0, to_ = 0, value_ = 0;
    uint32_t started_ = 0;
    uint16_t duration_ = 0;
    Easing easing_ = Easing::Smoothstep;
    bool active_ = false;
};

// AstraUI-style target following: each update advances by a fraction of the
// remaining distance, producing a fast start and a long settling tail.
class FollowTween {
public:
    static bool step(float& value, float target, float speed = 70.0f,
                     float snapDistance = 1.0f) {
        const float distance = target - value;
        if (fabsf(distance) <= snapDistance) {
            value = target;
            return false;
        }
        float denominator = 100.0f - speed;
        if (denominator < 1.0f) denominator = 1.0f;
        value += distance / denominator;
        return true;
    }

    static bool stepWrapped(float& value, float target, int count,
                            float speed = 70.0f, float snapDistance = 0.01f) {
        if (count <= 1) return step(value, target, speed, snapDistance);
        float distance = target - value;
        const float half = static_cast<float>(count) / 2.0f;
        while (distance > half) distance -= static_cast<float>(count);
        while (distance < -half) distance += static_cast<float>(count);
        if (fabsf(distance) <= snapDistance) {
            value = target;
            return false;
        }
        float denominator = 100.0f - speed;
        if (denominator < 1.0f) denominator = 1.0f;
        value += distance / denominator;
        if (value < 0.0f) value += static_cast<float>(count);
        if (value >= static_cast<float>(count)) value -= static_cast<float>(count);
        return true;
    }
};

} // namespace pixelpad
