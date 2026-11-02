#include <Arduino.h>
#include <esp_ota_ops.h>
#include "Bluetooth.h"
#include "led.h"
#include "button.h"
#include "OTA.h"
#include "GameOfLife.h"
#include "Pacman.h"
#include "SandTetris.h"
#include "app.h"
#include "Runtime.h"

Bluetooth ble;
LED ledDisplay;
MyControl myControl;
GameOfLife lifeGame;
PacmanGame pacmanGame;
SandTetrisGame sandGame;
App appManager;

void setup() {
    Serial.begin(115200);
    ledDisplay.setupPins();
    ledDisplay.clearScreen();
    setupOTA();
    if (!ble.init()) { Serial.println("Fatal: BLE queue allocation failed"); abort(); }
    appManager.begin();
    lifeGame.begin();
    pacmanGame.begin();
    sandGame.begin();
    myControl.begin();
    ledDisplay.setBrightness(255);
    appManager.playBootAnim();
    pixelpad::startRuntime();
    const esp_err_t validation = esp_ota_mark_app_valid_cancel_rollback();
    if (validation != ESP_OK && validation != ESP_ERR_NOT_FOUND)
        Serial.printf("OTA app validation: %s\n", esp_err_to_name(validation));
    Serial.printf("PixelPad RTOS 3.2.4: ESP-IDF %s, heap=%u, PSRAM=%u\n",
                  ESP.getSdkVersion(), ESP.getFreeHeap(), ESP.getFreePsram());
}

void loop() {
    // Arduino bootstraps ESP-IDF; application work is in explicit FreeRTOS tasks.
    vTaskDelay(portMAX_DELAY);
}
