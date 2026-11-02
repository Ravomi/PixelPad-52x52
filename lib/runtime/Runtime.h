#pragma once

#include <stdint.h>

namespace pixelpad {
enum class Input : uint8_t { Click, DoubleClick, WhiteOn, WhiteOff, Plus, Minus };
// Called only by the input sampler; never touches application/display state.
bool postInput(Input input);
uint32_t inputSession();
void startRuntime();
} // namespace pixelpad
