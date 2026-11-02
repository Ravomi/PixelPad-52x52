#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fm6373c {
constexpr unsigned kWidth = 52, kHeight = 52, kRows = 26;
constexpr unsigned kChips = 4, kChannels = 16, kGroups = 64;
constexpr unsigned kClockHz = 8000000, kLineClocks = 80;
constexpr unsigned kHeaderClocks = 32; // VSYNC(3), then 29 low clocks (>=16).
constexpr unsigned kDataSlots = kRows * kChannels;
// 16 groups of data + one group containing register maintenance and idle.
constexpr unsigned kMutableSlots = 17 * kRows;
constexpr unsigned kMutableWords = kMutableSlots * kLineClocks;
constexpr unsigned kIdleWords = kRows * kLineClocks;
constexpr unsigned kIdleGroups = kGroups - 17;
constexpr unsigned kFrameWords = kHeaderClocks + kRows * kGroups * kLineClocks;
constexpr unsigned kFrameBytes = kWidth * kHeight * 3;
// 24 slots = 3840 bytes: each PSRAM DMA segment is 64-byte aligned.
constexpr unsigned kDataChunks = (kMutableSlots + 23) / 24;
constexpr unsigned kDescriptorCount = 1 + kDataChunks + kIdleGroups * 2;
constexpr uint16_t kLe = 1u << 6, kRow = 1u << 7;
constexpr unsigned kAddressShift = 8;
constexpr uint16_t kGrayMax = 8191;

enum class Command : uint8_t {
    DataLatch = 1, Vsync = 3, WriteConfig = 5, ReadConfig = 7,
    SoftReset = 9, Enable = 11, Disable = 12, PreActive = 14
};
struct Register { uint8_t address, rgb[3]; };
enum class Storage { Header, Mutable, Idle };
struct Segment { Storage storage; unsigned offset, words; };
Segment frameSegment(unsigned index);
extern const Register kRegisters[];
extern const size_t kRegisterCount;

// Physical shift stage 0 is nearest SDI. The old driver sent x=0 first
// and x=51 last. Stages 52..63 therefore have no visible pixel.
// Centralized for adjustment after the first physical column test.
int logicalX(unsigned chip, unsigned channel);
unsigned logicalY(unsigned row, unsigned half);
uint16_t gray(uint8_t value, uint8_t brightness);

// Each word represents the six SDI bits, LE, ROW and A..E at one DCLK
// rising edge. All encoders are independent of Arduino and DMA.
void command(uint16_t* words, unsigned clocks, Command cmd,
             const uint16_t (&data)[kChips][6]);
void control(uint16_t* words, unsigned clocks, Command cmd);
void writeRegister(uint16_t* words, const Register& reg);
void header(uint16_t* words);
void idleGroup(uint16_t* words);
void frame(uint16_t* words, const uint8_t* rgb, uint8_t brightness,
           unsigned maintenanceRegister = 0);
size_t initializationWords();
void initialization(uint16_t* words);
// Diagnostic phase: rows, columns, individual RGB lanes, bars, grayscale.
void diagnostic(uint8_t* rgb, unsigned phase);
} // namespace fm6373c
