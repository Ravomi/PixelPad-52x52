#pragma once
#include <cstdint>
#include <cstdlib>
extern uint32_t hostMillis;
inline uint32_t millis() { return hostMillis; }
