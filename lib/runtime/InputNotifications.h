#pragma once
#include "Runtime.h"
#include <stddef.h>
#include <stdint.h>

namespace pixelpad {
struct InputNotification {
    Input input;
    uint32_t sequence, session, rotarySession;
    int32_t region;
};

// Render-task owned. Phone synchronization has a separate bounded FIFO so
// a stalled BLE link never prevents the next physical input being applied.
class InputNotifications {
public:
    static constexpr size_t Capacity = 64;
    void push(const InputNotification& event) {
        if (size_ == Capacity) {
            // Keep the newest board state during prolonged disconnection or
            // congestion. Loss is observable separately from physical inputs.
            pop();
            ++drops_;
        }
        events_[(head_ + size_) % Capacity] = event;
        ++size_;
    }
    const InputNotification& front() const { return events_[head_]; }
    void pop() { if (size_) { head_ = (head_ + 1) % Capacity; --size_; } }
    size_t size() const { return size_; }
    uint32_t drops() const { return drops_; }
    void clear() { head_ = size_ = 0; }
private:
    InputNotification events_[Capacity]{};
    size_t head_ = 0, size_ = 0;
    uint32_t drops_ = 0;
};
}
