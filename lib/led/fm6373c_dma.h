#pragma once
#include "fm6373c_protocol.h"
#include <Arduino.h>
#include <esp_private/gdma.h>
#include <hal/dma_types.h>

namespace fm6373c {
// Only the display task writes inactive frames or requests a switch.
// DMA owns an entire bank until an EOF on the OTHER bank's header.
class Dma {
public:
    bool begin();
    void end();
    bool present(unsigned bank, bool enableOnce = false);
    bool waitFrames(unsigned bank, unsigned count, uint32_t timeoutMs = 1000);
    unsigned active();
    uint32_t frames();
    uint16_t* buffer(unsigned bank) { return banks_[bank].data; }
    size_t allocatedBytes() const;
    static void disableOutputs();

private:
    static constexpr unsigned kDescriptors = kDescriptorCount;
    struct Bank {
        uint16_t* data = nullptr;
        dma_descriptor_t* desc = nullptr;
    } banks_[2];
    uint16_t* common_ = nullptr; // header, idle group, one-shot EN_OP
    uint16_t* init_ = nullptr;
    dma_descriptor_t* initDesc_ = nullptr;
    dma_descriptor_t* enableDesc_ = nullptr;
    gdma_channel_handle_t channel_ = nullptr;
    bool peripheral_ = false, connected_ = false;
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
    volatile int active_ = -1;
    volatile uint32_t counts_[2] = {0,0};
    static bool IRAM_ATTR eof(gdma_channel_handle_t, gdma_event_data_t*, void*);
    bool configure();
    void linkBank(unsigned bank);
};
} // namespace fm6373c
