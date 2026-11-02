#include "led.h"
#include "DisplayConfig.h"
#include <esp_timer.h>

void LED::setupPins() {
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
    frame.set(x, y, r, g, b);
}

void LED::clearScreen() { frame.clear(); }
void LED::turnOff() { clearScreen(); }

void LED::refreshDisplay() {
    // Legacy App/game hook. Hardware scans continuously; Runtime::renderTask
    // presents the final frame, after all drawing for this iteration completes.
}

void LED::present() {
    if (!dma_display || !frame.changed()) return;
    const uint32_t start = static_cast<uint32_t>(esp_timer_get_time());
    if (pixelpad::kDoubleBuffer && !pixelpad::dmaBufferReusable(
            start, lastFlipUs, dma_display->calculated_refresh_rate, hasFlipped)) return;

    dmaPixelWrites += frame.flush(backBuffer, [this](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
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
}

void LED::writeStats(char* text, size_t size) const {
    snprintf(text, size,
        "{\"status\":\"ok\",\"cmd\":\"display_stats\",\"clock_hz\":%lu,\"depth_bits\":%u,"
        "\"double_buffer\":%s,\"scan_hz_est\":%d,\"presented\":%lu,\"pixel_writes\":%lu,"
        "\"present_max_us\":%lu,\"uptime_ms\":%lu}\n",
        (unsigned long)pixelpad::kPanelClockHz, pixelpad::kPanelDepthBits,
        pixelpad::kDoubleBuffer ? "true" : "false",
        dma_display ? dma_display->calculated_refresh_rate : 0,
        (unsigned long)presentedFrames, (unsigned long)dmaPixelWrites,
        (unsigned long)maxPresentUs, (unsigned long)millis());
}

void LED::setBrightness(uint8_t value) {
    if (dma_display) {
        if (value == 255) brightnessDivider = 1;
        dma_display->setBrightness8(pixelpad::scaleBrightness(value));
    }
}
