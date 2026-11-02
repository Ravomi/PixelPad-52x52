#pragma once
#include <Arduino.h>
#include "ProtocolLimits.h"

class Bluetooth {
public:
    bool init();
    String getDeviceName() const;
    bool saveDeviceName(const String& name);
    bool readJsonLine(char* output, size_t outputSize);
    int readLargeData(uint8_t* buffer, int size, uint32_t idleTimeoutMs,
                      uint32_t totalTimeoutMs, uint32_t session);
    bool connected() const;
    uint32_t session() const;
    uint32_t readSession() const { return readSession_; }
    bool receiveHealthy(uint32_t session) const;
    void invalidateReceive(uint32_t session);
    // Copies a complete newline-terminated reply. The service task alone owns TX.
    // countDrop=false is retriable input traffic and reserves four reply slots.
    bool notify(const char* text, uint32_t session = 0, bool countDrop = true);
    void service();
    uint32_t rxOverflows() const;
    uint32_t rxOversize() const;
    uint32_t rxPeak() const;
    uint32_t txDrops() const;
private:
    bool initialized_ = false;
    uint32_t readSession_ = 0;
    pixelpad::JsonLine line_;
    struct Packet {
        uint32_t session;
        uint16_t size;
        uint8_t bytes[512];
    } packet_{};
    size_t packetOffset_ = 0;
    int readBytes(uint8_t* output, size_t capacity, uint32_t expectedSession, uint32_t waitMs);
    friend class RxCallback;
};
