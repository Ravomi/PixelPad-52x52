#pragma once
#include "Runtime.h"
#include <stddef.h>
#include <stdint.h>

namespace pixelpad {
// Input-task owned FIFO. Compress only adjacent turns in the same direction;
// reversal and button boundaries must survive runtime/TX backpressure.
template<size_t Capacity> class InputBacklog {
public:
    bool push(Input input, uint32_t count = 1) {
        if (!count) return true;
        if (size_ && (input == Input::Plus || input == Input::Minus)) {
            Run& last = runs_[(head_ + size_ - 1) % Capacity];
            if (last.input == input && count <= UINT32_MAX - last.count) {
                last.count += count;
                pending_ += count;
                return true;
            }
        }
        if (size_ == Capacity) return false;
        runs_[(head_ + size_) % Capacity] = {input, count};
        ++size_;
        pending_ += count;
        return true;
    }
    bool empty() const { return size_ == 0; }
    Input front() const { return runs_[head_].input; }
    void pop() {
        if (empty()) return;
        --pending_;
        if (--runs_[head_].count == 0) {
            head_ = (head_ + 1) % Capacity;
            --size_;
        }
    }
    uint64_t pending() const { return pending_; }
private:
    struct Run { Input input; uint32_t count; };
    Run runs_[Capacity]{};
    size_t head_ = 0, size_ = 0;
    uint64_t pending_ = 0;
};
} // namespace pixelpad
