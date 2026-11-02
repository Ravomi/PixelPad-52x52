#pragma once
#include <cstdint>
#include <cassert>
struct HUB75_I2S_CFG {
    struct Pins { int r1,g1,b1,r2,g2,b2,a,b,c,d,e,clk,lat,oe; } gpio{};
    enum clk_speed { HZ_16M = 16000000 };
    enum Driver { FM6124 };
    clk_speed i2sspeed = HZ_16M;
    Driver driver = FM6124;
    bool clkphase = false, double_buff = false;
    unsigned min_refresh_rate = 0, latch_blanking = 0;
    HUB75_I2S_CFG(unsigned w, unsigned h, unsigned n) { assert(w==52 && h==52 && n==1); }
    void setPixelColorDepthBits(unsigned) {}
};
class MatrixPanel_I2S_DMA {
public:
    static MatrixPanel_I2S_DMA* instance;
    uint8_t buffers[2][52*52*3]{};
    unsigned active = 0, back = 1, flips = 0;
    int calculated_refresh_rate = 303;
    explicit MatrixPanel_I2S_DMA(const HUB75_I2S_CFG& config) {
        assert(config.driver == HUB75_I2S_CFG::FM6124);
        assert(config.gpio.oe == 3 && config.gpio.clk == 18);
        instance = this;
    }
    bool begin() { return true; }
    void setBrightness8(uint8_t) {}
    void drawPixelRGB888(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        assert(x>=0 && x<52 && y>=0 && y<52);
        auto* p = buffers[back]+(y*52+x)*3;
        p[0]=r; p[1]=g; p[2]=b;
    }
    void flipDMABuffer() { active=back; back^=1; ++flips; }
};
