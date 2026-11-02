#include "led.h"

void LED::setupPins() {
    // 1. 配置 HUB75 结构体
    HUB75_I2S_CFG mxconfig(
    52,   // 宽度 (虽然你只用52，但通常模组是64宽)
    52,   // 高度
        1     // 链数
    );
    // 2. 映射你自己的引脚
    mxconfig.gpio.r1 = R1_PIN; mxconfig.gpio.g1 = G1_PIN; mxconfig.gpio.b1 = B1_PIN;
    mxconfig.gpio.r2 = R2_PIN; mxconfig.gpio.g2 = G2_PIN; mxconfig.gpio.b2 = B2_PIN;
    mxconfig.gpio.a = ADDR_A_PIN; mxconfig.gpio.b = ADDR_B_PIN; 
    mxconfig.gpio.c = ADDR_C_PIN; mxconfig.gpio.d = ADDR_D_PIN; 
    mxconfig.gpio.e = ADDR_E_PIN;
    mxconfig.gpio.clk = CLK_PIN; mxconfig.gpio.lat = LAT_PIN; mxconfig.gpio.oe = OE_PIN;

//    pinMode(47, OUTPUT);    
//    digitalWrite(47, HIGH); // 确保供电开启


   delay(2000);


    // 3. 针对 ESP32-S3 的特殊配置
    mxconfig.clkphase = false;
// 1. 降低 DMA 吐数据的时钟频率，让信号飞得稳一点 (默认是 10M 或 20M)
    // 52x52 定制屏在 20MHz 下容易出现颜色通道采样错误和横向拖影。
    // 使用稳定档位，牺牲少量刷新余量换取信号完整性。
    mxconfig.i2sspeed = HUB75_I2S_CFG::HZ_8M;
    
    // 2. 增加锁存消隐时间（Latch Blanking）
    // 这相当于在每次换行时，多等几个时钟周期，专门等那根“慢半拍”的 47 引脚切换到位！
    mxconfig.latch_blanking = 2;

    // 如果你的屏幕显示不正常（如重影），可以尝试修改驱动芯片类型
    // mxconfig.driver = HUB75_I2S_CFG::FM6126A; 
    mxconfig.driver = HUB75_I2S_CFG::FM6124;
    // 4. 启动 DMA 驱动
    dma_display = new MatrixPanel_I2S_DMA(mxconfig);
    if (!dma_display->begin()) {
        Serial.println("Fatal: HUB75 DMA initialization failed");
        abort();
    }

    // pinMode(47, OUTPUT); // 必须先把它设为输出模式
    
    // // 抓取 DMA 库刚刚分配给 E 引脚的高速信号外设索引
    // uint32_t e_signal_idx = GPIO.func_out_sel_cfg[ADDR_E_PIN].func_sel;
    
    // // 强制将该高速信号镜像输出到 47 引脚，并且设置反相 (invert = true)
    // gpio_matrix_out(47, e_signal_idx, true, false);


    brightnessDivider = 1;
    setBrightness(255);
    dma_display->clearScreen();
    
    Serial.println("✨ DMA LED 驱动初始化完成");
}

// void LED::setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
//     // 保护边界，防止越界崩溃
//     if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
//     if (!dma_display) return;

//     int x_phys = (WIDTH - 1) - x; 

//     int y_phys = y;

//     // 直接透传给底层驱动
//     dma_display->drawPixelRGB888(x_phys, y_phys, r, g, b);
// }


void LED::setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
    if (!dma_display) return;

    // x = (WIDTH - 1) - x;
    y = (HEIGHT - 1) - y; 

    int x_phys;
    int y_phys;

    if (y < 26) {

        x_phys = x;
        // y_phys =   51 - y;
        y_phys =   25 - y;
        // y_phys =   y;
    } else {

        x_phys = x;   
        // y_phys = 51-y;
        y_phys = 77 - y;
        //  y_phys =   y;

    }




//     // 将计算好的物理坐标传给 DMA 库
    dma_display->drawPixelRGB888(x_phys, y_phys, r, g, b);
}

void LED::clearScreen() {
    if (dma_display) dma_display->fillScreenRGB888(0, 0, 0);
}

void LED::refreshDisplay() {
    // ★ 关键点：
    // 在 DMA 模式下，硬件会自动以极高频率刷新屏幕（通常是几千Hz）。
    // 所以你不需要再手动写 row 和 bit 的循环。
    // 这个函数现在变为空，保证你原来的代码 loop 里调用它不会报错。
}

void LED::turnOff() {
    if (dma_display) dma_display->fillScreenRGB888(0, 0, 0);
}



void LED::setBrightness(uint8_t value) {
    if (dma_display) {
        // DMA 库自带的设置亮度函数
        if (value == 255) {
            brightnessDivider = 1;
        }
        dma_display->setBrightness8(pixelpad::scaleBrightness(value));
    }
}
