#ifndef LED_H
#define LED_H

#include <Arduino.h>
#include "brightness_policy.h"

class LED {
public:
    static const uint8_t WIDTH = 52;
    static const uint8_t HEIGHT = 52;
    void setupPins();
    void setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b);
    void clearScreen();
    void refreshDisplay();
    void present(uint16_t fadeMs = 0); // Commit; optionally transition locally.
    void writeStats(char* text, size_t size) const;
    void turnOff();
    void changeBrightness(int delta);
    int getBrightness();
    void setBrightness(uint8_t value);
    bool isReady() const;

private:
    struct State;
    State* state_ = nullptr;
    int brightnessDivider = 1;
    static void displayTask(void* arg);
};
#endif
