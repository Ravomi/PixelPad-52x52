#pragma once
#include <Arduino.h>

namespace fm6373c {
// The board defaults Serial to native USB CDC. This board is being tested
// through a USB-TTL adapter, so display diagnostics explicitly use UART0.
inline HardwareSerial& logPort() {
#if ARDUINO_USB_CDC_ON_BOOT
    return Serial0;
#else
    return Serial;
#endif
}
} // namespace fm6373c
