// Host protocol model: c++ -std=c++11 -Wall -Wextra -Werror -Ilib/led
// test/fm6373c_protocol_host.cpp lib/led/fm6373c_protocol.cpp -o /tmp/fm6373c-test
#include "fm6373c_protocol.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cmath>
#include <vector>
using namespace fm6373c;

struct Event {
    unsigned clocks;
    std::array<std::array<uint16_t, 6>, 4> data;
};
// Independent four-IC serial-chain model. Latch on LE falling BEFORE
// processing the next rising DCLK, including across descriptor boundaries.
struct Receiver {
    std::array<std::array<uint16_t, 6>, 4> shift = {};
    std::vector<Event> events;
    unsigned high = 0;
    void consume(const uint16_t* words, size_t count) {
        for (size_t t = 0; t < count; ++t) {
            if (!(words[t] & 0x40) && high) {
                events.push_back({high, shift}); high = 0;
            }
            if (words[t] & 0x40) ++high;
            for (unsigned lane = 0; lane < 6; ++lane) {
                unsigned bit = (words[t] >> lane) & 1;
                for (unsigned chip = 0; chip < 4; ++chip) {
                    const unsigned next = shift[chip][lane] >> 15;
                    shift[chip][lane] = (shift[chip][lane] << 1) | bit;
                    bit = next;
                }
            }
        }
    }
};
static std::vector<uint16_t> assemble(const std::vector<uint16_t>& mutableWords) {
    std::vector<uint16_t> h(kHeaderClocks), idle(kIdleWords), full;
    header(h.data()); idleGroup(idle.data());
    for (unsigned d = 0; d < kDescriptorCount; ++d) {
        const Segment s = frameSegment(d);
        assert(s.words > 0 && s.words * 2 <= 4092);
        assert((s.words * 2) % 4 == 0 && (s.offset * 2) % 4 == 0);
        if (s.storage == Storage::Mutable)
            assert((s.words * 2) % 64 == 0 && (s.offset * 2) % 64 == 0);
        const auto& source = s.storage == Storage::Header ? h : s.storage == Storage::Idle ? idle : mutableWords;
        assert(s.offset + s.words <= source.size());
        full.insert(full.end(), source.begin() + s.offset, source.begin() + s.offset + s.words);
    }
    assert(full.size() == 133152);
    assert(frameSegment(kDescriptorCount).words == 0);
    return full;
}
static void commandsAndRegisters() {
    uint16_t data[4][6];
    for (unsigned chip = 0; chip < 4; ++chip)
        for (unsigned lane = 0; lane < 6; ++lane) data[chip][lane] = 0x1234 + 0x100 * chip + lane;
    std::vector<uint16_t> words(80, 0x1280);
    command(words.data(), words.size(), Command::WriteConfig, data);
    Receiver r;
    // Deliberately split while LE is high and at its falling edge.
    r.consume(words.data(), 62); r.consume(words.data()+62, 2); r.consume(words.data()+64, 16);
    assert(r.events.size() == 1 && r.events[0].clocks == 5);
    for (unsigned chip = 0; chip < 4; ++chip) for (unsigned lane = 0; lane < 6; ++lane)
        assert(r.events[0].data[chip][lane] == data[chip][lane]);
    for (uint16_t w : words) assert((w & ~0x7f) == 0x1280);
    for (unsigned count : {3u, 9u, 11u, 12u, 14u}) {
        control(words.data(), words.size(), static_cast<Command>(count));
        Receiver c; c.consume(words.data(), words.size());
        assert(c.events.size() == 1 && c.events[0].clocks == count);
    }
    words.resize(initializationWords()); initialization(words.data());
    Receiver init;
    init.consume(words.data(), 2000); init.consume(words.data()+2000, words.size()-2000);
    assert(init.events.size() == 41);
    assert(init.events[0].clocks == 12 && init.events[1].clocks == 14);
    assert(init.events[2].data[0][0] == 0x00aa);
    assert(init.events[3].data[0][0] == 0x01aa);
    assert(init.events[39].data[0][0] == 0x0055);
    assert(init.events[40].data[0][0] == 0x0155);
    for (size_t i = 2; i < init.events.size(); ++i) assert(init.events[i].clocks == 5);
    // V1.2 literals, including color-specific and reserved bits.
    const struct { unsigned address, r, g, b; } expected[] = {
        {0x02,0x19,0x19,0x19}, {0x03,0x3f,0x3f,0x3f}, {0x07,0x20,0x20,0x20},
        {0x0f,1,1,1}, {0x16,0,0,0}, {0x18,0x0f,0x1f,0x1f},
        {0x19,0xc1,0x11,0x41}, {0x1c,0xd0,0xd0,0xd0}, {0x1e,0x52,0x56,0x58},
        {0x1f,2,0x20,0x20}, {0x20,0x44,0x46,0x48}
    };
    for (const auto& e : expected) {
        bool found = false;
        for (size_t i = 4; i < 39; ++i) {
            const auto& d = init.events[i].data;
            if ((d[0][0] >> 8) != e.address) continue;
            found = true;
            for (unsigned chip = 0; chip < 4; ++chip) for (unsigned half = 0; half < 2; ++half) {
                assert(d[chip][half*3] == (e.address << 8 | e.r));
                assert(d[chip][half*3+1] == (e.address << 8 | e.g));
                assert(d[chip][half*3+2] == (e.address << 8 | e.b));
            }
        }
        assert(found);
    }
}
static void scanAndMapping() {
    std::vector<uint8_t> pixels(kFrameBytes, 0);
    // Independent inverse of original driver's y transform.
    pixels[(0*52+0)*3] = 255;
    pixels[(25*52+51)*3+1] = 255;
    pixels[(26*52+0)*3+2] = 255;
    pixels[(51*52+51)*3] = 255;
    std::vector<uint16_t> encoded(kMutableWords);
    frame(encoded.data(), pixels.data(), 255);
    auto full = assemble(encoded);
    Receiver r;
    // Stream the exact segmented frame, exercising a split every DMA chunk.
    size_t offset = 0;
    for (unsigned d = 0; d < kDescriptorCount; ++d) {
        const unsigned count = frameSegment(d).words;
        r.consume(full.data()+offset, count); offset += count;
    }
    assert(r.events.size() == 424); // VSYNC + 416 data + PRE + 6 config
    assert(r.events[0].clocks == 3);
    for (unsigned i = 1; i <= 416; ++i) assert(r.events[i].clocks == 1);
    // x0 -> chip3 OUT3; x51 -> chip0 OUT0. RGB1 receives logical y26..51.
    assert(r.events[1+12].data[3][3] == 7355);
    assert(r.events[1+25*16+15].data[0][4] == 7355);
    assert(r.events[1+12].data[3][2] == 7355);
    assert(r.events[1+25*16+15].data[0][0] == 7355);
    unsigned lit = 0;
    for (unsigned i = 1; i <= 416; ++i)
        for (const auto& chip : r.events[i].data) for (auto v : chip) lit += v != 0;
    assert(lit == 4);
    // All 52 logical columns occur once; stage slots above 51 remain zero.
    bool seen[52] = {};
    for (unsigned chip = 0; chip < 4; ++chip) for (unsigned ch = 0; ch < 16; ++ch) {
        int x = logicalX(chip, ch);
        if (x < 0) continue;
        assert(!seen[x]); seen[x] = true;
    }
    for (bool present : seen) assert(present);
    unsigned pulses = 0, width = 0;
    bool wasHigh = false;
    for (size_t t = 0; t < full.size(); ++t) {
        const bool high = full[t] & 0x80;
        if (high && !wasHigh) {
            assert(t == 32 + pulses * 80);
            assert(((full[t] >> 8) & 31) == pulses % 26);
            ++pulses; width = 0;
        }
        if (high) ++width;
        if (!high && wasHigh) assert(width == (pulses == 1 ? 12u : 4u));
        wasHigh = high;
    }
    assert(pulses == 1664);
    assert(r.events[417].clocks == 14);
    assert(r.events[418].data[0][0] == 0x00aa);
    assert(r.events[419].data[0][0] == 0x01aa);
    assert(r.events[421].data[3][3] == 0x1308); // a non-black physical IC
    assert(r.events[421].data[2][3] == 0x1318); // a black physical IC
    assert(r.events[422].data[0][0] == 0x0055);
    assert(r.events[423].data[0][0] == 0x0155);
    // A second frame resets the SRAM write pointer and repeats ROW12 once.
    r.consume(full.data(), full.size());
    assert(r.events.size() == 848 && r.events[424].clocks == 3);
    assert(gray(255,255) == 7355 && gray(255,0) == 0);
    for (unsigned b = 0; b < 256; ++b) {
        uint16_t last = 0;
        for (unsigned v = 0; v < 256; ++v) {
            const uint16_t current = gray(v,b);
            assert(current >= last && current <= 7355); last = current;
        }
    }
    // Brightness zero must transmit actual black, including power-save bits.
    frame(encoded.data(), pixels.data(), 0, 13);
    full = assemble(encoded); Receiver off; off.consume(full.data(), full.size());
    for (unsigned i = 1; i <= 416; ++i)
        for (const auto& chip : off.events[i].data) for (auto v : chip) assert(v == 0);
    for (const auto& chip : off.events[421].data) for (auto v : chip) assert(v == 0x1318);
    assert(off.events[420].data[0][0] == 0x0219); // skip reg13 in rotation
    // Each RGB lane is addressed independently; omitted outputs always black.
    for (unsigned lane = 0; lane < 6; ++lane) {
        diagnostic(pixels.data(), 104+lane);
        frame(encoded.data(), pixels.data(), 255);
        full = assemble(encoded); Receiver single; single.consume(full.data(), full.size());
        unsigned count = 0;
        for (unsigned i = 1; i <= 416; ++i)
            for (const auto& chip : single.events[i].data)
                for (unsigned l = 0; l < 6; ++l) {
                    if (chip[l]) { assert(l == lane); ++count; }
                }
        assert(count == 52*26);
    }
}
static void srgbTransferCurve() {
    // Reference: IEC sRGB decoding curve, before linear LED PWM scaling.
    const double full = gray(255, 255);
    assert(std::abs(gray(128, 255) / full - 0.2158605) < 0.001);
    for (unsigned value = 0; value < 256; ++value) {
        const double encoded = value / 255.0;
        const double linear = encoded <= 0.04045
            ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
        assert(std::abs(gray(value, 255) - linear * full) <= 1.5);
    }
}
static void optimizedDataMatchesGenericEncoder() {
    std::vector<uint8_t> pixels(kFrameBytes);
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = (i * 73 + 19) & 255;
    std::vector<uint16_t> fast(kMutableWords);
    frame(fast.data(), pixels.data(), 173);
    for (unsigned slot = 0; slot < kDataSlots; ++slot) {
        uint16_t reference[kLineClocks];
        for (unsigned t = 0; t < kLineClocks; ++t)
            reference[t] = ((slot % kRows) << kAddressShift)
                         | (t < (slot == 0 ? 12u : 4u) ? kRow : 0);
        uint16_t data[kChips][6] = {};
        for (unsigned chip = 0; chip < kChips; ++chip) {
            const int x = logicalX(chip, 15-slot%16);
            if (x < 0) continue;
            for (unsigned lane = 0; lane < 6; ++lane) {
                const unsigned y = logicalY(slot/16, lane/3);
                data[chip][lane] = gray(pixels[(y*kWidth+x)*3+lane%3], 173);
            }
        }
        command(reference, kLineClocks, Command::DataLatch, data);
        for (unsigned t = 0; t < kLineClocks; ++t)
            assert(reference[t] == fast[slot*kLineClocks+t]);
    }
}
int main() {
    srgbTransferCurve();
    optimizedDataMatchesGenericEncoder();
    commandsAndRegisters(); scanAndMapping();
    puts("FM6373C protocol: commands, V1.2 registers, chain mapping, descriptor boundaries, scan, RGB and brightness PASS");
}
