#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pixelpad {
// Mount/load at boot; only the protocol task accesses storage after startup.
void mountStandbyStore();
uint8_t* loadStandby(int& frames, int& fps);
bool saveStandby(const uint8_t* data, size_t bytes, int frames, int fps);
bool removeStandby();
} // namespace pixelpad
