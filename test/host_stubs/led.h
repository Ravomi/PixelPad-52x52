#pragma once
#include <cstdint>
struct LED {
    unsigned clears = 0, pixels = 0;
    int lastX = -1, lastY = -1;
    void clearScreen() { ++clears; }
    void setPixel(int x, int y, uint8_t, uint8_t, uint8_t) { ++pixels; lastX = x; lastY = y; }
};
