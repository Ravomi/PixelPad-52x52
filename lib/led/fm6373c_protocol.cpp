#include "fm6373c_protocol.h"
#include "brightness_policy.h"
#include "srgb_gray_lut.h"
#include <string.h>

namespace fm6373c {
// FM6373C programming guide V1.2, final register table. Reserved bits
// are intentionally preserved, including differences between R/G/B.
const Register kRegisters[] = {
    {0x02,{0x19,0x19,0x19}}, {0x03,{0x3f,0x3f,0x3f}},
    {0x04,{0x02,0x02,0x02}}, {0x05,{0x08,0x08,0x08}},
    {0x06,{0x02,0x02,0x02}}, {0x07,{0x20,0x20,0x20}},
    {0x0c,{0x0a,0x10,0x10}}, {0x0d,{0x01,0x01,0x01}},
    {0x0e,{0x04,0x04,0x04}}, {0x0f,{0x01,0x01,0x01}},
    {0x10,{0xc2,0xc2,0xc2}}, {0x11,{0x21,0x21,0x21}},
    {0x12,{0x01,0x01,0x01}}, {0x13,{0x08,0x08,0x08}},
    {0x14,{0x00,0x00,0x00}}, {0x15,{0x00,0x00,0x00}},
    {0x16,{0x00,0x00,0x00}}, {0x17,{0xf0,0xf0,0xf0}},
    {0x18,{0x0f,0x1f,0x1f}}, {0x19,{0xc1,0x11,0x41}},
    {0x1a,{0x1f,0x1f,0x1f}}, {0x1b,{0x10,0x10,0x10}},
    {0x1c,{0xd0,0xd0,0xd0}}, {0x1d,{0x52,0x52,0x52}},
    {0x1e,{0x52,0x56,0x58}}, {0x1f,{0x02,0x20,0x20}},
    {0x20,{0x44,0x46,0x48}}, {0x21,{0x01,0x01,0x01}},
    {0x22,{0x1c,0x1c,0x1c}}, {0x23,{0x00,0x00,0x00}},
    {0x70,{0x00,0x00,0x00}}, {0x71,{0x00,0x00,0x00}},
    {0x72,{0x00,0x00,0x00}}, {0x73,{0x00,0x00,0x00}},
    {0x74,{0x00,0x00,0x00}}
};
const size_t kRegisterCount = sizeof(kRegisters) / sizeof(kRegisters[0]);

Segment frameSegment(unsigned index) {
    if (index == 0) return {Storage::Header, 0, kHeaderClocks};
    if (index <= kDataChunks) {
        const unsigned slot = (index - 1) * 24;
        const unsigned count = kMutableSlots - slot < 24 ? kMutableSlots - slot : 24;
        return {Storage::Mutable, slot * kLineClocks, count * kLineClocks};
    }
    if (index >= kDescriptorCount) return {Storage::Idle, 0, 0};
    return (index - 1 - kDataChunks) % 2 == 0
        ? Segment{Storage::Idle, 0, 25*kLineClocks}
        : Segment{Storage::Idle, 25*kLineClocks, kLineClocks};
}

int logicalX(unsigned chip, unsigned channel) {
    const int x = 51 - static_cast<int>(chip * 16 + channel);
    return x >= 0 && x < 52 ? x : -1;
}
unsigned logicalY(unsigned row, unsigned half) {
    // Algebraic simplification of the existing led.cpp transform:
    // logical y<26 -> RGB2; logical y>=26 -> RGB1, same row index.
    return row + (half == 0 ? 26 : 0);
}
uint16_t gray(uint8_t value, uint8_t brightness) {
    return static_cast<uint32_t>(kSrgbToGray[value])
           * pixelpad::scaleBrightness(brightness) / 255u;
}

void command(uint16_t* words, unsigned clocks, Command cmd,
             const uint16_t (&data)[kChips][6]) {
    // Place LE across the last N bits, then explicitly deassert it. The
    // caller initializes ROW/address first; this function preserves them.
    const unsigned count = static_cast<unsigned>(cmd);
    if (clocks < kChips * 16 + 1) return;
    for (unsigned t = 0; t < clocks; ++t) {
        uint16_t value = words[t] & ~uint16_t(0x7f);
        if (t < 64) {
            const unsigned chip = 3 - t / 16, bit = 15 - t % 16;
            for (unsigned lane = 0; lane < 6; ++lane)
                value |= ((data[chip][lane] >> bit) & 1u) << lane;
            if (t >= 64 - count) value |= kLe;
        }
        words[t] = value;
    }
}
void control(uint16_t* words, unsigned clocks, Command cmd) {
    const unsigned count = static_cast<unsigned>(cmd);
    for (unsigned t = 0; t < clocks; ++t)
        words[t] = (words[t] & ~uint16_t(0x7f)) | (t < count ? kLe : 0);
}
void writeRegister(uint16_t* words, const Register& reg) {
    uint16_t data[kChips][6] = {};
    for (unsigned chip = 0; chip < kChips; ++chip)
        for (unsigned lane = 0; lane < 6; ++lane)
            data[chip][lane] = (uint16_t(reg.address) << 8) | reg.rgb[lane % 3];
    command(words, kLineClocks, Command::WriteConfig, data);
}
static void scanSlot(uint16_t* words, unsigned slot, bool first) {
    const uint16_t address = (slot % kRows) << kAddressShift;
    for (unsigned t = 0; t < kLineClocks; ++t)
        words[t] = address | (t < (first ? 12u : 4u) ? kRow : 0);
}
void header(uint16_t* words) {
    // Retain last row through frame synchronization. The first slot sets A..E.
    for (unsigned t = 0; t < kHeaderClocks; ++t)
        words[t] = (kRows - 1) << kAddressShift;
    control(words, kHeaderClocks, Command::Vsync);
}
void idleGroup(uint16_t* words) {
    for (unsigned slot = 0; slot < kRows; ++slot)
        scanSlot(words + slot * kLineClocks, slot, false);
}
static Register password(uint8_t address, uint8_t value) {
    return {address, {value, value, value}};
}
static void dataSlot(uint16_t* words, unsigned slot,
                     const uint16_t (&data)[kChips][6]) {
    const uint16_t address = (slot % kRows) << kAddressShift;
    for (unsigned t = 0; t < kLineClocks; ++t) {
        uint16_t value = address | (t < (slot == 0 ? 12u : 4u) ? kRow : 0);
        if (t < 64) {
            const uint16_t* d = data[3 - t / 16];
            const unsigned bit = 15 - t % 16;
            value |= ((d[0] >> bit) & 1u)
                   | (((d[1] >> bit) & 1u) << 1)
                   | (((d[2] >> bit) & 1u) << 2)
                   | (((d[3] >> bit) & 1u) << 3)
                   | (((d[4] >> bit) & 1u) << 4)
                   | (((d[5] >> bit) & 1u) << 5);
            if (t >= 64 - static_cast<unsigned>(Command::DataLatch)) value |= kLe;
        }
        // Write each PSRAM word once, without reading an intermediate scanSlot.
        words[t] = value;
    }
}
void frame(uint16_t* words, const uint8_t* rgb, uint8_t brightness,
           unsigned maintenanceRegister) {
    for (unsigned slot = kDataSlots; slot < kMutableSlots; ++slot)
        scanSlot(words + slot * kLineClocks, slot, slot == 0);
    bool black[kChips][6];
    memset(black, 1, sizeof(black));
    uint16_t lut[256];
    for (unsigned i = 0; i < 256; ++i) lut[i] = gray(i, brightness);
    for (unsigned slot = 0; slot < kDataSlots; ++slot) {
        const unsigned row = slot / 16, channel = 15 - slot % 16;
        uint16_t data[kChips][6] = {};
        for (unsigned chip = 0; chip < kChips; ++chip) {
            const int x = logicalX(chip, channel);
            if (x < 0 || !rgb) continue;
            for (unsigned lane = 0; lane < 6; ++lane) {
                const unsigned y = logicalY(row, lane / 3);
                data[chip][lane] = lut[rgb[(y * kWidth + x) * 3 + lane % 3]];
                if (data[chip][lane]) black[chip][lane] = false;
            }
        }
        dataSlot(words + slot * kLineClocks, slot, data);
    }
    // Quality-first power saving: refresh reg13 for each physical chip/lane
    // plus one other register, in one uninterrupted write-enabled block.
    uint16_t* cfg = words + kDataSlots * kLineClocks;
    control(cfg, kLineClocks, Command::PreActive);
    writeRegister(cfg + kLineClocks, password(0, 0xaa));
    writeRegister(cfg + 2*kLineClocks, password(1, 0xaa));
    Register rotating = kRegisters[maintenanceRegister % kRegisterCount];
    if (rotating.address == 0x13) rotating = kRegisters[0];
    writeRegister(cfg + 3*kLineClocks, rotating);
    uint16_t data[kChips][6];
    for (unsigned chip = 0; chip < kChips; ++chip)
        for (unsigned lane = 0; lane < 6; ++lane)
            data[chip][lane] = 0x1308 | (black[chip][lane] ? 0x10 : 0);
    command(cfg + 4*kLineClocks, kLineClocks, Command::WriteConfig, data);
    writeRegister(cfg + 5*kLineClocks, password(0, 0x55));
    writeRegister(cfg + 6*kLineClocks, password(1, 0x55));
}
size_t initializationWords() { return (kRegisterCount + 6) * kLineClocks; }
void initialization(uint16_t* words) {
    memset(words, 0, initializationWords() * sizeof(uint16_t));
    control(words, kLineClocks, Command::Disable);
    control(words + kLineClocks, kLineClocks, Command::PreActive);
    writeRegister(words + 2*kLineClocks, password(0, 0xaa));
    writeRegister(words + 3*kLineClocks, password(1, 0xaa));
    for (size_t i = 0; i < kRegisterCount; ++i)
        writeRegister(words + (4+i)*kLineClocks, kRegisters[i]);
    writeRegister(words + (4+kRegisterCount)*kLineClocks, password(0, 0x55));
    writeRegister(words + (5+kRegisterCount)*kLineClocks, password(1, 0x55));
}
void diagnostic(uint8_t* rgb, unsigned phase) {
    phase %= 52 + 52 + 6 + 2;
    memset(rgb, 0, kFrameBytes);
    for (unsigned y = 0; y < 52; ++y) for (unsigned x = 0; x < 52; ++x) {
        uint8_t* p = rgb + (y * 52 + x) * 3;
        if (phase < 52) { if (y == phase) p[0] = p[1] = p[2] = 64; }
        else if (phase < 104) { if (x == phase - 52) p[0] = p[1] = p[2] = 64; }
        else if (phase < 110) {
            unsigned lane = phase - 104;
            if ((y >= 26 ? 0u : 1u) == lane / 3) p[lane % 3] = 64;
        } else if (phase == 110) p[x * 3 / 52] = 64;
        else p[0] = p[1] = p[2] = x * 255 / 51;
    }
}
} // namespace fm6373c
