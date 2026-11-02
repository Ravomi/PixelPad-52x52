#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
extern uint32_t hostMs;
inline uint32_t millis() { return hostMs; }
inline void delay(uint32_t ms) { hostMs += ms; }
struct SerialStub {
    void println(const char*) {}
    template<class... Args> void printf(const char*, Args...) {}
};
extern SerialStub Serial;
