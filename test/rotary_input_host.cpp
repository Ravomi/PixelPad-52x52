#include "button.h"
#include "Runtime.h"
#include <cassert>
#include <cstdio>
#include <vector>

uint32_t hostMillis = 0;
int64_t hostEncoderCount = 0;
puType ESP32Encoder::useInternalWeakPullResistors = puType::up;
static bool blocked = false;
static uint32_t session = 1;
static std::vector<pixelpad::Input> events;
namespace pixelpad {
uint32_t inputSession() { return session; }
bool postInput(Input input) {
    if (blocked) return false;
    events.push_back(input);
    return true;
}
}
static void poll(MyControl& control, unsigned times = 100) {
    for (unsigned i = 0; i < times; ++i) {
        hostMillis += 2;
        control.loop();
    }
}
int main() {
    MyControl control;
    control.begin();
    // Ten complete detents between samples must produce ten events.
    hostEncoderCount = 20;
    poll(control);
    std::printf("burst: expected 10 steps, got %zu\n", events.size());
    std::fflush(stdout);
    assert(events.size() == 10);
    for (auto event : events) assert(event == pixelpad::Input::Plus);
    // Preserve a half detent across successful delivery.
    hostEncoderCount = 23;
    poll(control);
    assert(events.size() == 11);
    hostEncoderCount = 24;
    poll(control);
    assert(events.size() == 12);
    // Saturated runtime queue: all movement remains pending, including a
    // reversal while blocked. Expected FIFO is +5 then -3, not net +2.
    blocked = true;
    hostEncoderCount = 34;
    poll(control);
    hostEncoderCount = 28;
    poll(control);
    assert(events.size() == 12);
    blocked = false;
    poll(control);
    assert(events.size() == 20);
    for (size_t i = 12; i < 17; ++i)
        assert(events[i] == pixelpad::Input::Plus);
    for (size_t i = 17; i < 20; ++i)
        assert(events[i] == pixelpad::Input::Minus);
    // No artificial 35 ms gate: the first full detent is ready next sample.
    hostEncoderCount = 30;
    poll(control, 1);
    assert(events.size() == 21);
    blocked = true;
    hostEncoderCount += 8;
    poll(control);
    ++session;
    blocked = false;
    poll(control);
    assert(events.size() == 21); // No old-session replay after reconnect.
    pixelpad::InputBacklog<2> fifo;
    assert(fifo.push(pixelpad::Input::Plus, 1000));
    assert(fifo.push(pixelpad::Input::Click));
    assert(!fifo.push(pixelpad::Input::Minus));
    for (int i = 0; i < 1000; ++i) {
        assert(fifo.front() == pixelpad::Input::Plus);
        fifo.pop();
    }
    assert(fifo.front() == pixelpad::Input::Click);
    fifo.pop();
    assert(fifo.empty());
    // Replay 10,000 samples with fast reversals and periodic backpressure
    // through the actual MyControl::loop(), not a copy of its algorithm.
    MyControl stressed;
    stressed.begin();
    events.clear();
    std::vector<pixelpad::Input> expected;
    uint32_t seed = 0x6373;
    for (unsigned sample = 0; sample < 10000; ++sample) {
        seed = seed * 1664525u + 1013904223u;
        const int steps = int((seed >> 16) % 9) - 4;
        hostEncoderCount += steps * 2;
        for (int i = 0; i < std::abs(steps); ++i)
            expected.push_back(steps > 0 ? pixelpad::Input::Plus : pixelpad::Input::Minus);
        blocked = sample % 100 < 20;
        poll(stressed, 1);
    }
    blocked = false;
    poll(stressed);
    assert(events == expected);
    assert(stressed.backlogFull() == 0);
    std::printf("PASS stress: %zu ordered steps across 10000 samples\n", events.size());
    std::puts("PASS rotary_input: burst, half-detent, FIFO retry, first-step latency");
}
