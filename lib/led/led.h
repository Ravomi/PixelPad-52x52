#ifndef LED_H
#define LED_H

#include <Arduino.h>
#include "brightness_policy.h"
#include "PixelFrame.h"
#include "PublishedFrame.h"
#include "FrameTransition.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h> // 引入 DMA 库

class LED {
public:
    static const uint8_t WIDTH = 52;
    static const uint8_t HEIGHT = 52;
    
private:
    MatrixPanel_I2S_DMA *dma_display = nullptr;
    // CPU-owned RGB staging lives in PSRAM; DMA bitplanes stay in internal RAM.
    pixelpad::PixelFrame* frame = nullptr;
    struct Presentation {
        pixelpad::PublishedFrame<WIDTH, HEIGHT> target;
        pixelpad::FrameTransition<WIDTH * HEIGHT * 3> transition;
        uint8_t displayed[WIDTH * HEIGHT * 3]{};
        bool pending = false;
        uint32_t committed = 0, shown = 0, shownHash = 0, transitionFrames = 0;
    };
    Presentation* presentation = nullptr;
    unsigned backBuffer = 1; // HUB75 begin() displays buffer 0, draws buffer 1.
    uint32_t lastFlipUs = 0;
    bool hasFlipped = false;
    uint32_t presentedFrames = 0, dmaPixelWrites = 0, maxPresentUs = 0;
    int brightnessDivider = 1; // 1-10 范围，1 为最高亮度

    // 引脚定义保持不变
    static const uint8_t R1_PIN = 5;
    static const uint8_t G1_PIN = 6;
    static const uint8_t B1_PIN = 7;
    static const uint8_t R2_PIN = 15;
    static const uint8_t G2_PIN = 16;
    static const uint8_t B2_PIN = 17;
    static const uint8_t ADDR_A_PIN = 4;
    static const uint8_t ADDR_B_PIN = 21;
    static const uint8_t ADDR_C_PIN = 14;
    static const uint8_t ADDR_D_PIN = 2;
    static const uint8_t ADDR_E_PIN = 1;
    static const uint8_t CLK_PIN = 18;
    static const uint8_t LAT_PIN = 8;
    static const uint8_t OE_PIN = 3;



    //    // 测试引脚
    // static const uint8_t R1_PIN = 5;
    // static const uint8_t G1_PIN = 6;
    // static const uint8_t B1_PIN = 7;
    // static const uint8_t R2_PIN = 15;
    // static const uint8_t G2_PIN = 16;
    // static const uint8_t B2_PIN = 17;
    // static const uint8_t ADDR_A_PIN = 4;
    // static const uint8_t ADDR_B_PIN = 21;
    // static const uint8_t ADDR_C_PIN = 14;
    // static const uint8_t ADDR_D_PIN = 2;
    // static const uint8_t ADDR_E_PIN = 1;
    // static const uint8_t CLK_PIN = 18;
    // static const uint8_t LAT_PIN = 8;
    // static const uint8_t OE_PIN = 19;

public:
    void setupPins();
    void setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b);
    void clearScreen();
    void refreshDisplay(); // 在 DMA 模式下，此函数将变为空或仅处理特殊逻辑
    bool isReady() const { return dma_display && frame && presentation; }
    bool present(uint16_t fadeMs = 0); // False while hardware presentation is deferred.
    bool submitFrame(const uint8_t* rgb, uint16_t fadeMs = 0);
    void writeStats(char* text, size_t size) const;
    void turnOff();

    void changeBrightness(int delta) {
        brightnessDivider += delta;
        if (brightnessDivider < 1) brightnessDivider = 1;
        if (brightnessDivider > 10) brightnessDivider = 10;
        
        // 实时更新 DMA 亮度 (255 是最大)
        if (dma_display) {
            setBrightness(static_cast<uint8_t>(255 / brightnessDivider));
        }
    }

    int getBrightness() { return brightnessDivider; }

    void setBrightness(uint8_t value);

};

#endif
