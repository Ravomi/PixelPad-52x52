#include "led.h"
#include "esp_heap_caps.h"
#include <cassert>
#include <cstring>
uint32_t hostMs = 0;
SerialStub Serial;
std::vector<void*> allocations;
MatrixPanel_I2S_DMA* MatrixPanel_I2S_DMA::instance = nullptr;
int main() {
    LED led;
    led.setupPins();
    assert(led.isReady());
    auto* dma = MatrixPanel_I2S_DMA::instance;
    auto channel = [&](int x, int y, int c) {
        return dma->buffers[dma->active][((y<26?y+26:y-26)*52+x)*3+c];
    };
    led.setPixel(0,0,200,0,0);
    led.setPixel(51,51,0,200,0);
    assert(led.present());
    assert(channel(0,0,0)==200 && channel(51,51,1)==200);
    // A target commit while the old DMA buffer is guarded must remain pending.
    led.setPixel(0,0,0,0,200);
    assert(!led.present(200)); // Phone notifications must wait for DMA readiness.
    assert(channel(0,0,0)==200);
    hostMs+=100;
    assert(led.present());
    assert(channel(0,0,0)==100 && channel(0,0,2)==100);
    assert(channel(51,51,1)==200); // Unchanged pixels survive both buffer swaps.
    // Retarget a running fade from the displayed intermediate frame.
    led.setPixel(0,0,0,200,0);
    led.present(200);
    hostMs+=100;
    led.present();
    assert(channel(0,0,0)==50 && channel(0,0,1)==100 && channel(0,0,2)==50);
    hostMs+=100;
    led.present();
    assert(channel(0,0,0)==0 && channel(0,0,1)==200 && channel(0,0,2)==0);
    const unsigned flips = dma->flips;
    hostMs+=100;
    led.present();
    assert(dma->flips==flips); // Idle frames do not keep rewriting DMA.
    led.clearScreen();
    led.setPixel(1,1,20,30,40);
    led.refreshDisplay();
    assert(channel(0,0,1)==200); // Legacy refresh cannot publish a partial draw.
    led.present();
    assert(channel(0,0,1)==0 && channel(1,1,2)==40);
    uint8_t menu[52*52*3]{};
    menu[(26*52+26)*3] = 90;
    assert(led.submitFrame(menu,180));
    hostMs+=180;
    led.present();
    assert(channel(26,26,0)==90 && channel(1,1,2)==0);
    const unsigned menuFlips = dma->flips;
    assert(led.submitFrame(menu));
    assert(dma->flips==menuFlips); // Same menu snapshot does not restart/rewrite a fade.
    char stats[512];
    led.writeStats(stats,sizeof(stats));
    assert(std::strstr(stats,"\"driver\":\"fm6124\""));
    assert(std::strstr(stats,"\"fading\":false"));
    for (void* p : allocations) std::free(p);
    delete dma;
}
