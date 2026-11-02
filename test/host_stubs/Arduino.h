#pragma once
#include <cstdint>
#include <cstring>
#include <string>
using String = std::string;
extern unsigned long hostMillis;
inline unsigned long millis() { return hostMillis; }
struct SerialStub { void println(const char*) {} };
extern SerialStub Serial;
