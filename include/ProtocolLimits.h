#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pixelpad {
constexpr size_t kFrameBytes = 52 * 52 * 3;
constexpr int kMaxStandbyFrames = 256;
constexpr int kMaxStandbyFps = 60;
constexpr size_t kJsonBytes = 1024; // Includes the terminating NUL.

inline bool validStandby(int frames, int fps, size_t bytes) {
    // Bound the multiplicand before multiplying, including on 32-bit targets.
    return frames > 0 && frames <= kMaxStandbyFrames &&
           fps > 0 && fps <= kMaxStandbyFps &&
           bytes == static_cast<size_t>(frames) * kFrameBytes;
}

// Accept a whole line or discard through its newline. Never execute a suffix
// of an oversized command, and never consume binary bytes after that newline.
class JsonLine {
public:
    enum class Result { Incomplete, Complete, TooLong };
    Result push(uint8_t ch, char* output, size_t capacity) {
        if (ch == '\r') return Result::Incomplete;
        if (ch == '\n') {
            const bool rejected = overflow_ || length_ >= capacity;
            if (!rejected && length_ && output) {
                for (size_t i = 0; i < length_; ++i) output[i] = data_[i];
                output[length_] = '\0';
            }
            const bool hasLine = length_ != 0;
            reset();
            return rejected ? Result::TooLong :
                   (hasLine ? Result::Complete : Result::Incomplete);
        }
        if (!overflow_) {
            if (length_ < sizeof(data_) - 1) data_[length_++] = ch;
            else overflow_ = true;
        }
        return Result::Incomplete;
    }
    void reset() { length_ = 0; overflow_ = false; }
private:
    char data_[kJsonBytes]{};
    size_t length_ = 0;
    bool overflow_ = false;
};
} // namespace pixelpad
