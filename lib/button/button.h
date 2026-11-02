#ifndef BUTTON_H
#define BUTTON_H

#include <Arduino.h>
#include "OneButton.h"
#include <ESP32Encoder.h>
#include "InputBacklog.h"
#include <atomic>

// 定义引脚
#define ENC_CLK 40 //B
#define ENC_DT  41 //A
#define ENC_SW  42

class MyControl {
public:
    MyControl(); // 构造函数
    void begin();
    void loop();
    uint32_t pendingInputs() const { return pendingCount_.load(); }
    uint32_t backlogPeak() const { return backlogPeak_.load(); }
    uint32_t backlogFull() const { return backlogFull_.load(); }
    uint32_t buttonDrops() const { return buttonDrops_.load(); }

private:
    ESP32Encoder encoder;
    OneButton button;

    int64_t oldPosition;
    uint32_t lastSession_ = 0;
    pixelpad::InputBacklog<64> pending_;
    std::atomic<uint32_t> pendingCount_{0}, backlogPeak_{0}, backlogFull_{0}, buttonDrops_{0};
    static MyControl* activeControl_;
    static void enqueueButton(pixelpad::Input input);

    // 静态回调函数 (OneButton 需要静态函数或全局函数)
    static void handleClick();
    static void handleDoubleClick();
    // ★ 把原来的 handleLongPress 换成下面这两个：
    static void handleLongPressStart();
    static void handleLongPressStop();

};

#endif
