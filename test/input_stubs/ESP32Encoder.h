#pragma once
#include <cstdint>
enum class puType { up };
extern int64_t hostEncoderCount;
class ESP32Encoder {
public:
    static puType useInternalWeakPullResistors;
    void attachHalfQuad(int, int) {}
    void setCount(int64_t value) { hostEncoderCount = value; }
    int64_t getCount() const { return hostEncoderCount; }
};
