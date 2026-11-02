#include "led.h"
#include "fm6373c_dma.h"
#include "fm6373c_log.h"
#include "PublishedFrame.h"
#include "FrameTransition.h"
#include <esp_heap_caps.h>
#include <freertos/semphr.h>
#include <new>
#include <string.h>

#ifndef FM6373C_DIAGNOSTIC
#define FM6373C_DIAGNOSTIC 0
#endif

struct LED::State {
    fm6373c::Dma dma;
    SemaphoreHandle_t mutex = nullptr;
    TaskHandle_t task = nullptr;
    pixelpad::PublishedFrame<WIDTH, HEIGHT> frame;
    uint8_t snapshot[fm6373c::kFrameBytes] = {};
    pixelpad::FrameTransition<fm6373c::kFrameBytes> transition;
    uint16_t fadeMs = 0;
    bool fading = false;
    uint32_t transitionFrames = 0, encodeMaxUs = 0;
    uint8_t brightness = 255;
    bool dirty = true;
    bool running = false;
    uint32_t committed = 0, shown = 0, shownHash = 0;
};

void LED::setupPins() {
    if (state_) return;
    fm6373c::logPort().begin(115200);
    fm6373c::logPort().println("FM6373C: setup begin (UART0 diagnostics)");
    fm6373c::Dma::disableOutputs();
    // Keep ISR state, RGB buffers and synchronization objects in internal RAM.
    void* memory = heap_caps_malloc(sizeof(State), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!memory) { fm6373c::logPort().println("FM6373C: state allocation failed"); return; }
    State* s = new (memory) State;
    s->mutex = xSemaphoreCreateMutex();
    if (!s->mutex) { s->~State(); heap_caps_free(s); return; }
    delay(2000); // Retain the previous board's power stabilization delay.
    if (!s->dma.begin()) {
        vSemaphoreDelete(s->mutex); s->~State(); heap_caps_free(s); return;
    }
    state_ = s;
    s->running = true;
    if (xTaskCreate(displayTask, "fm6373c", 4096, this, 1, &s->task) != pdPASS) {
        s->running = false;
        s->dma.end(); vSemaphoreDelete(s->mutex);
        state_ = nullptr; s->~State(); heap_caps_free(s);
        fm6373c::logPort().println("FM6373C: display task allocation failed"); return;
    }
    fm6373c::logPort().printf("FM6373C ready: 52x52, 26 scan, 4 IC/lane, DCLK=%u Hz, line=%u clocks, frame=%.2f Hz, DMA=%u bytes\n",
        fm6373c::kClockHz, fm6373c::kLineClocks,
        float(fm6373c::kClockHz) / fm6373c::kFrameWords, unsigned(s->dma.allocatedBytes()));
    fm6373c::logPort().printf("FM6373C: internal heap free=%u, largest=%u; diagnostic=%d\n",
        unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)), FM6373C_DIAGNOSTIC);
    fm6373c::logPort().printf("FM6373C: waveform PSRAM=%u bytes; UART commands: fm test / fm normal / fm status\n",
        2 * fm6373c::kMutableWords * 2);
}

bool LED::isReady() const {
    if (!state_) return false;
    xSemaphoreTake(state_->mutex, portMAX_DELAY);
    const bool ready = state_->running;
    xSemaphoreGive(state_->mutex);
    return ready;
}

void LED::setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (!state_ || x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
    State& s = *state_;
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    s.frame.setPixel(x, y, r, g, b);
    xSemaphoreGive(s.mutex);
}

void LED::clearScreen() {
    if (!state_) return;
    xSemaphoreTake(state_->mutex, portMAX_DELAY);
    state_->frame.clear();
    xSemaphoreGive(state_->mutex);
    refreshDisplay();
}
void LED::turnOff() { clearScreen(); } // Black pixels; clock and row scan keep running.
void LED::refreshDisplay() {
    // App/game calls can occur halfway through drawing. Only present() commits.
}
void LED::present(uint16_t fadeMs) {
    if (!state_) return;
    xSemaphoreTake(state_->mutex, portMAX_DELAY);
    if (state_->frame.commit()) {
        state_->fadeMs = fadeMs > 500 ? 500 : fadeMs;
        state_->dirty = true;
        ++state_->committed;
    }
    xSemaphoreGive(state_->mutex);
    if (state_ && state_->task) xTaskNotifyGive(state_->task);
}
void LED::writeStats(char* text, size_t size) const {
    if (!state_) { snprintf(text, size, "{\"cmd\":\"display_stats\",\"status\":\"err\"}\n"); return; }
    xSemaphoreTake(state_->mutex, portMAX_DELAY);
    snprintf(text, size,
        "{\"cmd\":\"display_stats\",\"status\":\"ok\",\"driver\":\"fm6373c\","
        "\"width\":52,\"height\":52,\"clock_hz\":%u,\"frames\":%u,\"ready\":%s,"
        "\"committed\":%u,\"shown\":%u,\"shown_hash\":%u,"
        "\"fading\":%s,\"transition_frames\":%u,\"encode_max_us\":%u}\n",
        unsigned(fm6373c::kClockHz), unsigned(state_->dma.frames()), state_->running ? "true" : "false",
        unsigned(state_->committed), unsigned(state_->shown), unsigned(state_->shownHash),
        state_->fading ? "true" : "false", unsigned(state_->transitionFrames), unsigned(state_->encodeMaxUs));
    xSemaphoreGive(state_->mutex);
}
void LED::setBrightness(uint8_t value) {
    if (!state_) return;
    xSemaphoreTake(state_->mutex, portMAX_DELAY);
    if (value == 255) brightnessDivider = 1;
    state_->brightness = value; state_->dirty = true;
    xSemaphoreGive(state_->mutex);
    refreshDisplay();
}
void LED::changeBrightness(int delta) {
    if (!state_) return;
    xSemaphoreTake(state_->mutex, portMAX_DELAY);
    const int64_t next = int64_t(brightnessDivider) + delta;
    brightnessDivider = next < 1 ? 1 : next > 10 ? 10 : int(next);
    state_->brightness = uint8_t(255 / brightnessDivider); state_->dirty = true;
    xSemaphoreGive(state_->mutex);
    refreshDisplay();
}
int LED::getBrightness() {
    if (!state_) return brightnessDivider;
    xSemaphoreTake(state_->mutex, portMAX_DELAY);
    const int value = brightnessDivider;
    xSemaphoreGive(state_->mutex);
    return value;
}

void LED::displayTask(void* arg) {
    State& s = *static_cast<LED*>(arg)->state_;
    unsigned maintenance = 0;
    uint32_t lastEncode = 0, lastReport = millis();
    uint32_t previousFrames = s.dma.frames();
    uint32_t observedRevision = 0;
    bool diagnosticEnabled = FM6373C_DIAGNOSTIC;
    uint32_t diagnosticStart = millis();
    unsigned previousPhase = 112;
    char input[24] = {};
    unsigned inputLength = 0;
    for (;;) {
        // Notifications coalesce. Even when the app blocks (or runs OTA),
        // dirty pixels are picked up within 16 ms and DMA never stops.
        // DMA bank handoff paces active transitions at the hardware frame rate.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s.transition.active() ? 1 : 16));
        const uint32_t now = millis();
        // Local UART diagnostics only; does not change the BLE protocol.
        while (fm6373c::logPort().available()) {
            const char c = fm6373c::logPort().read();
            if (c == '\r') continue;
            if (c == '\n') {
                input[inputLength] = 0;
                if (strcmp(input, "fm test") == 0 || strcmp(input, "fm normal") == 0) {
                    diagnosticEnabled = strcmp(input, "fm test") == 0;
                    diagnosticStart = now; previousPhase = 112;
                    xSemaphoreTake(s.mutex, portMAX_DELAY); s.dirty = true; xSemaphoreGive(s.mutex);
                    fm6373c::logPort().printf("FM6373C: diagnostic=%d\n", diagnosticEnabled);
                } else if (strcmp(input, "fm status") == 0) {
                    fm6373c::logPort().printf("FM6373C: frames=%u, bank=%u, diagnostic=%d\n",
                        unsigned(s.dma.frames()), s.dma.active(), diagnosticEnabled);
                }
                inputLength = 0;
            } else if (inputLength < sizeof(input)-1) input[inputLength++] = c;
            else inputLength = 0;
        }
        if (!s.transition.active() && now - lastEncode < 16) { vTaskDelay(1); continue; }
        xSemaphoreTake(s.mutex, portMAX_DELAY);
        bool update = s.dirty || s.transition.active() || now - lastEncode >= 250;
        uint8_t brightness = s.brightness;
        const uint32_t revision = s.committed;
        if (revision != observedRevision) {
            // snapshot is the last frame handed to DMA, including an unfinished fade.
            s.transition.begin(s.snapshot, now, diagnosticEnabled ? 0 : s.fadeMs);
            observedRevision = revision;
        }
        if (update) { s.frame.snapshot(s.snapshot); s.dirty = false; }
        xSemaphoreGive(s.mutex);
        if (update) s.transition.apply(s.snapshot, now);
        if (diagnosticEnabled) {
            const unsigned phase = ((now - diagnosticStart) / 600) % 112;
            if (phase != previousPhase) {
                fm6373c::logPort().printf("FM6373C diagnostic phase=%u (rows 0-51, cols 52-103, lanes 104-109, bars 110, gray 111)\n", phase);
                previousPhase = phase;
            }
            fm6373c::diagnostic(s.snapshot, phase);
            brightness = 255;
            update = true;
        }
        if (update) {
            const unsigned target = 1 - s.dma.active();
            const uint32_t encodeStart = micros();
            fm6373c::frame(s.dma.buffer(target), s.snapshot, brightness, maintenance++);
            const uint32_t encodeUs = micros() - encodeStart;
            if (!s.dma.present(target)) {
                fm6373c::logPort().println("FM6373C: DMA bank switch timed out; disabling outputs");
                s.dma.end();
                xSemaphoreTake(s.mutex, portMAX_DELAY);
                s.running = false;
                xSemaphoreGive(s.mutex);
                // Keep this task handle valid for callers; no writes to freed DMA.
                for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
            }
            // Report the RGB snapshot only after the target DMA bank is
            // active. Scan FPS alone cannot detect a stuck image update.
            uint32_t hash = 2166136261u;
            for (uint8_t byte : s.snapshot) hash = (hash ^ byte) * 16777619u;
            xSemaphoreTake(s.mutex, portMAX_DELAY);
            s.fading = s.transition.active();
            if (s.fading) ++s.transitionFrames;
            else s.shown = revision;
            if (encodeUs > s.encodeMaxUs) s.encodeMaxUs = encodeUs;
            s.shownHash = hash;
            xSemaphoreGive(s.mutex);
            lastEncode = millis();
        }
        if (now - lastReport >= 10000) {
            const uint32_t currentFrames = s.dma.frames();
            fm6373c::logPort().printf("FM6373C: DMA %.2f frames/s, internal heap=%u\n",
                float(currentFrames - previousFrames) * 1000 / (now - lastReport),
                unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
            previousFrames = currentFrames; lastReport = now;
        }
    }
}
