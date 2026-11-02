#include "led.h"
#include "DisplayConfig.h"
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <new>

void LED::setupPins() {
    void* staging = heap_caps_malloc(sizeof(pixelpad::PixelFrame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!staging) {
        Serial.println("Fatal: RGB staging allocation in PSRAM failed");
        abort();
    }
    frame = new (staging) pixelpad::PixelFrame();
    void* storage = heap_caps_malloc(sizeof(Presentation), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!storage) { Serial.println("Fatal: presentation allocation failed"); abort(); }
    presentation = new (storage) Presentation();
    HUB75_I2S_CFG mxconfig(WIDTH, HEIGHT, 1);
    mxconfig.gpio.r1 = R1_PIN; mxconfig.gpio.g1 = G1_PIN; mxconfig.gpio.b1 = B1_PIN;
    mxconfig.gpio.r2 = R2_PIN; mxconfig.gpio.g2 = G2_PIN; mxconfig.gpio.b2 = B2_PIN;
    mxconfig.gpio.a = ADDR_A_PIN; mxconfig.gpio.b = ADDR_B_PIN;
    mxconfig.gpio.c = ADDR_C_PIN; mxconfig.gpio.d = ADDR_D_PIN;
    mxconfig.gpio.e = ADDR_E_PIN;
    mxconfig.gpio.clk = CLK_PIN; mxconfig.gpio.lat = LAT_PIN; mxconfig.gpio.oe = OE_PIN;
    delay(2000);

    mxconfig.clkphase = false;
    // Match the S3 hardware divider and the driver's scan-rate calculation.
    // HZ_8M/HZ_10M are both 8M enums, but S3 3.0.15 outputs 10M by default.
    mxconfig.i2sspeed = static_cast<HUB75_I2S_CFG::clk_speed>(pixelpad::kPanelClockHz);
    mxconfig.min_refresh_rate = pixelpad::kMinScanHz;
    mxconfig.setPixelColorDepthBits(pixelpad::kPanelDepthBits);
    mxconfig.double_buff = pixelpad::kDoubleBuffer;
    mxconfig.latch_blanking = 2;
    mxconfig.driver = HUB75_I2S_CFG::FM6124;
    dma_display = new MatrixPanel_I2S_DMA(mxconfig);
    if (!dma_display->begin()) {
        Serial.println("Fatal: HUB75 DMA initialization failed");
        abort();
    }
    backBuffer = pixelpad::kDoubleBuffer ? 1 : 0;
    brightnessDivider = 1;
    setBrightness(255);
    // begin() initializes both DMA buffers to black; the staged frame is black.
    Serial.printf("HUB75: clock=%lu Hz, depth=%u bits/channel, double_buffer=%u, scan_est=%d Hz\n",
                  (unsigned long)pixelpad::kPanelClockHz, pixelpad::kPanelDepthBits,
                  pixelpad::kDoubleBuffer, dma_display->calculated_refresh_rate);
}

void LED::setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (presentation) presentation->target.setPixel(x, y, r, g, b);
}

void LED::clearScreen() { if (presentation) presentation->target.clear(); }
void LED::turnOff() { clearScreen(); }

void LED::refreshDisplay() {
    // Legacy App/game hook. Hardware scans continuously; Runtime::renderTask
    // presents the final frame, after all drawing for this iteration completes.
}

bool LED::present(uint16_t fadeMs) {
    if (!isReady()) return false;
    if (presentation->target.commit()) {
        presentation->transition.begin(presentation->displayed, millis(), fadeMs);
        presentation->pending = true;
        ++presentation->committed;
    }
    if (!presentation->pending && !presentation->transition.active()) return true;
    const uint32_t start = static_cast<uint32_t>(esp_timer_get_time());
    if (pixelpad::kDoubleBuffer && !pixelpad::dmaBufferReusable(
            start, lastFlipUs, dma_display->calculated_refresh_rate, hasFlipped)) return false;

    presentation->target.snapshot(presentation->displayed);
    presentation->transition.apply(presentation->displayed, millis());
    presentation->pending = false;
    uint32_t hash = 2166136261u;
    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            const uint8_t* rgb = presentation->displayed + (y * WIDTH + x) * 3;
            frame->set(x, y, rgb[0], rgb[1], rgb[2]);
            for (unsigned c = 0; c < 3; ++c) hash = (hash ^ rgb[c]) * 16777619u;
        }
    }
    presentation->shownHash = hash;
    if (presentation->transition.active()) ++presentation->transitionFrames;
    else presentation->shown = presentation->committed;
    if (!frame->changed()) return !presentation->transition.active();
    dmaPixelWrites += frame->flush(backBuffer, [this](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        // Same custom 52x52 mapping as the original firmware.
        const int yPhys = y < 26 ? y + 26 : y - 26;
        dma_display->drawPixelRGB888(x, yPhys, r, g, b);
    });
    if (pixelpad::kDoubleBuffer) {
        dma_display->flipDMABuffer();
        lastFlipUs = static_cast<uint32_t>(esp_timer_get_time());
        hasFlipped = true;
        backBuffer ^= 1;
    }
    ++presentedFrames;
    const uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time()) - start;
    if (elapsed > maxPresentUs) maxPresentUs = elapsed;
    return true;
}

bool LED::submitFrame(const uint8_t* rgb, uint16_t fadeMs) {
    if (!isReady() || !rgb) return false;
    presentation->target.copy(rgb);
    present(fadeMs);
    return true;
}

void LED::writeStats(char* text, size_t size) const {
    snprintf(text, size,
        "{\"status\":\"ok\",\"cmd\":\"display_stats\",\"driver\":\"fm6124\",\"clock_hz\":%lu,\"depth_bits\":%u,"
        "\"double_buffer\":%s,\"scan_hz_est\":%d,\"presented\":%lu,\"pixel_writes\":%lu,"
        "\"present_max_us\":%lu,\"uptime_ms\":%lu,"
        "\"committed\":%lu,\"shown\":%lu,\"shown_hash\":%lu,"
        "\"fading\":%s,\"transition_frames\":%lu}\n",
        (unsigned long)pixelpad::kPanelClockHz, pixelpad::kPanelDepthBits,
        pixelpad::kDoubleBuffer ? "true" : "false",
        dma_display ? dma_display->calculated_refresh_rate : 0,
        (unsigned long)presentedFrames, (unsigned long)dmaPixelWrites,
        (unsigned long)maxPresentUs, (unsigned long)millis(),
        (unsigned long)(presentation ? presentation->committed : 0),
        (unsigned long)(presentation ? presentation->shown : 0),
        (unsigned long)(presentation ? presentation->shownHash : 0),
        presentation && presentation->transition.active() ? "true" : "false",
        (unsigned long)(presentation ? presentation->transitionFrames : 0));
}

void LED::setBrightness(uint8_t value) {
    if (dma_display) {
        if (value == 255) brightnessDivider = 1;
        dma_display->setBrightness8(pixelpad::scaleBrightness(value));
    }
}
