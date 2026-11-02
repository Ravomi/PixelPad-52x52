#pragma once

#include <stdint.h>

// These values must also be passed to the HUB75 library at compile time.
// Its CIE lookup table is selected by PIXEL_COLOR_DEPTH_BITS, not by setCfg().
#ifndef PIXEL_COLOR_DEPTH_BITS
#error "Define PIXEL_COLOR_DEPTH_BITS globally in platformio.ini"
#endif
#ifndef S3_LCD_DIV_NUM
#error "Define the actual ESP32-S3 LCD clock divider in platformio.ini"
#endif
#ifndef PIXELPAD_SCAN_MIN_HZ
#define PIXELPAD_SCAN_MIN_HZ 240
#endif
#ifndef PIXELPAD_DOUBLE_BUFFER
#define PIXELPAD_DOUBLE_BUFFER 1
#endif
#ifdef SPIRAM_DMA_BUFFER
#error "PixelPad scan timing requires internal DMA buffers"
#endif

namespace pixelpad {
constexpr uint32_t kPanelClockHz = 160000000UL / S3_LCD_DIV_NUM;
constexpr uint8_t kPanelDepthBits = PIXEL_COLOR_DEPTH_BITS;
constexpr uint8_t kMinScanHz = PIXELPAD_SCAN_MIN_HZ;
constexpr bool kDoubleBuffer = PIXELPAD_DOUBLE_BUFFER != 0;
static_assert(S3_LCD_DIV_NUM == 10 || S3_LCD_DIV_NUM == 16,
              "Use the 16 MHz candidate or the original 10 MHz clock");
static_assert(PIXEL_COLOR_DEPTH_BITS == 4 || PIXEL_COLOR_DEPTH_BITS == 8,
              "Use a native CIE LUT for the supported display profiles");
// HUB75 3.0.15 stores min_refresh_rate in uint8_t, despite a uint16_t ctor.
static_assert(PIXELPAD_SCAN_MIN_HZ >= 1 && PIXELPAD_SCAN_MIN_HZ <= 255,
              "HUB75 min_refresh_rate would overflow");
} // namespace pixelpad
