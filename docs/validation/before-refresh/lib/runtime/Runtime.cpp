#include "Runtime.h"
#include "ProtocolLimits.h"
#include "StandbyStore.h"
#include "Bluetooth.h"
#include "BleOTA.h"
#include "OTA.h"
#include "button.h"
#include "led.h"
#include "app.h"
#include "GameOfLife.h"
#include "Pacman.h"
#include "SandTetris.h"
#include <ArduinoJson.h>
#include <atomic>
#include <esp_timer.h>
#include <freertos/queue.h>

extern Bluetooth ble;
extern LED ledDisplay;
extern MyControl myControl;
extern App appManager;
extern GameOfLife lifeGame;
extern PacmanGame pacmanGame;
extern SandTetrisGame sandGame;

namespace pixelpad {
namespace {
static_assert(configTICK_RATE_HZ == 1000, "PixelPad input scheduling requires a 1 ms tick");
#if CONFIG_FREERTOS_UNICORE
#error "PixelPad RTOS requires the dual-core ESP32-S3 configuration"
#endif
struct InputEvent { Input input; uint32_t timeUs; };
struct Command {
    char json[kJsonBytes];
    uint8_t* data;
    size_t bytes;
    uint32_t session;
    uint32_t timeUs;
};
QueueHandle_t inputQueue = nullptr;
QueueHandle_t commandQueue = nullptr;
TaskHandle_t inputHandle, renderHandle, protocolHandle, serviceHandle;
std::atomic<uint32_t> inputDrops{0}, commandDrops{0}, restartAt{0};
// Statistics below are owned exclusively by the render task.
uint32_t inputCount = 0, commandCount = 0, maxInputUs = 0, maxCommandUs = 0, maxRenderUs = 0;
enum class Mode { Image, Life, Pacman, Sand };
Mode mode = Mode::Image;
bool allWhite = false;

uint32_t nowUs() { return static_cast<uint32_t>(esp_timer_get_time()); }
bool reply(const char* command, const char* status, const char* reason, uint32_t session) {
    JsonDocument doc;
    doc["status"] = status;
    doc["cmd"] = command;
    if (reason) doc["reason"] = reason;
    char text[256];
    const size_t length = serializeJson(doc, text, sizeof(text) - 2);
    text[length] = '\n'; text[length + 1] = 0;
    return ble.notify(text, session);
}

void sendStats(uint32_t session) {
    char text[512];
    snprintf(text, sizeof(text),
        "{\"status\":\"ok\",\"cmd\":\"rtos_stats\",\"input_events\":%lu,\"commands\":%lu,"
        "\"input_max_us\":%lu,\"command_max_us\":%lu,\"render_max_us\":%lu,"
        "\"rx_overflows\":%lu,\"tx_drops\":%lu,\"input_drops\":%lu,\"command_drops\":%lu,"
        "\"heap\":%u,\"min_heap\":%u,\"stack_free\":[%u,%u,%u,%u]}\n",
        (unsigned long)inputCount, (unsigned long)commandCount,
        (unsigned long)maxInputUs, (unsigned long)maxCommandUs, (unsigned long)maxRenderUs,
        (unsigned long)ble.rxOverflows(), (unsigned long)ble.txDrops(),
        (unsigned long)inputDrops.load(), (unsigned long)commandDrops.load(),
        ESP.getFreeHeap(), ESP.getMinFreeHeap(),
        (unsigned)uxTaskGetStackHighWaterMark(inputHandle),
        (unsigned)uxTaskGetStackHighWaterMark(renderHandle),
        (unsigned)uxTaskGetStackHighWaterMark(protocolHandle),
        (unsigned)uxTaskGetStackHighWaterMark(serviceHandle));
    ble.notify(text, session);
}

void handleInput(const InputEvent& event) {
    ++inputCount;
    maxInputUs = max(maxInputUs, nowUs() - event.timeUs);
    appManager.updateActivity();
    const char* name = "click";
    switch (event.input) {
        case Input::Click: name = "click"; break;
        case Input::DoubleClick: name = "dbl_click"; break;
        case Input::Plus: name = "color_plus"; break;
        case Input::Minus: name = "color_minus"; break;
        case Input::WhiteOn:
            name = "all_white_on";
            allWhite = true;
            appManager.stop();
            for (int y = 0; y < 52; ++y)
                for (int x = 0; x < 52; ++x) ledDisplay.setPixel(x, y, 255, 255, 255);
            break;
        case Input::WhiteOff:
            name = "all_white_off";
            allWhite = false; // The App resends the previous canvas, as before.
            break;
    }
    char text[64];
    snprintf(text, sizeof(text), "{\"evt\":\"%s\"}\n", name);
    ble.notify(text);
}

void handleCommand(Command& command) {
    if (!ble.receiveHealthy(command.session) || BleOTA::isInProgress()) return;
    ++commandCount;
    maxCommandUs = max(maxCommandUs, nowUs() - command.timeUs);
    JsonDocument doc;
    if (deserializeJson(doc, command.json)) return; // Already validated by the RX task.
    const char* cmd = doc["cmd"] | "";
    appManager.updateActivity();
    if (!strcmp(cmd, "rtos_stats")) {
        sendStats(command.session);
    } else if (!strcmp(cmd, "recv")) {
        appManager.stop();
        mode = Mode::Image;
        allWhite = false;
        for (int y = 0, i = 0; y < 52; ++y) {
            for (int x = 0; x < 52; ++x, i += 3)
                ledDisplay.setPixel(x, y, command.data[i], command.data[i + 1], command.data[i + 2]);
        }
        reply(cmd, "ok", nullptr, command.session);
    } else if (!strcmp(cmd, "set_standby")) {
        appManager.stop();
        appManager.adoptCustomStandby(doc["frames"].as<int>(), doc["fps"] | 8, command.data);
        command.data = nullptr; // Ownership moves to App; no second multi-MB copy.
        reply(cmd, "ok", nullptr, command.session);
    } else if (!strcmp(cmd, "clear_standby")) {
        appManager.stop();
        appManager.clearCustomStandby();
        reply(cmd, "ok", nullptr, command.session);
    } else if (!strcmp(cmd, "bright")) {
        ledDisplay.setBrightness(constrain(doc["val"] | 0, 0, 100) * 255 / 100);
    } else if (!strcmp(cmd, "standby")) {
        allWhite = false;
        appManager.playStandbyAnim();
    } else if (!strcmp(cmd, "mode")) {
        const char* value = doc["mode"] | "";
        if (!strcmp(value, "image")) { mode = Mode::Image; ledDisplay.clearScreen(); }
        else if (!strcmp(value, "life")) { mode = Mode::Life; lifeGame.begin(); }
        else if (!strcmp(value, "pacman")) { mode = Mode::Pacman; pacmanGame.begin(); }
        else if (!strcmp(value, "sand")) { mode = Mode::Sand; sandGame.begin(); }
        else { reply(cmd, "err", "invalid_mode", command.session); return; }
        allWhite = false;
        appManager.playWakeupAnim();
    } else if (!strcmp(cmd, "px_patch")) {
        // Stop a transition once; subsequent patches preserve untouched pixels.
        if (appManager.isAnimating()) appManager.stop();
        mode = Mode::Image;
        allWhite = false;
        for (JsonVariant op : doc["ops"].as<JsonArray>()) {
            if (!op.is<JsonArray>() || op.size() < 5) continue;
            const int x = op[0] | -1, y = op[1] | -1;
            ledDisplay.setPixel(x, y, constrain(op[2] | 0, 0, 255),
                                constrain(op[3] | 0, 0, 255), constrain(op[4] | 0, 0, 255));
        }
    } else if (!doc["game"].isNull() || !strcmp(cmd, "key")) {
        const char* key = doc["key"] | (doc["game"] | "");
        if (appManager.isAnimating()) appManager.stop();
        if (mode == Mode::Life) lifeGame.handleInput(key);
        if (mode == Mode::Pacman) pacmanGame.handleInput(key);
        if (mode == Mode::Sand) sandGame.handleInput(key);
    } else {
        reply(cmd, "err", "unknown_command", command.session);
    }
}

void renderTask(void*) {
    // Handles must all be initialized before statistics can be queried.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    for (;;) {
        const uint32_t start = nowUs();
        InputEvent event;
        for (unsigned i = 0; i < 8 && xQueueReceive(inputQueue, &event, 0) == pdTRUE; ++i) {
            if (!BleOTA::isInProgress()) handleInput(event);
        }
        Command command;
        for (unsigned i = 0; i < 4 && xQueueReceive(commandQueue, &command, 0) == pdTRUE; ++i) {
            handleCommand(command);
            free(command.data);
        }
        if (!BleOTA::isInProgress() && !allWhite) {
            appManager.loop();
            if (!appManager.isAnimating() && !appManager.isLogoState()) {
                switch (mode) {
                    case Mode::Image: break; // HUB75 DMA already refreshes continuously.
                    case Mode::Life: lifeGame.loop(); break;
                    case Mode::Pacman: pacmanGame.loop(); break;
                    case Mode::Sand: sandGame.loop(); break;
                }
            }
        }
        maxRenderUs = max(maxRenderUs, nowUs() - start);
        // Explicitly block even if a frame overruns. taskYIELD cannot feed idle.
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void inputTask(void*) {
    TickType_t lastWake = xTaskGetTickCount();
    for (;;) {
        myControl.loop(); // Samples only; no drawing, allocation or notification.
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(2));
        // Drop a missed schedule instead of spinning through catch-up scans.
        if (xTaskGetTickCount() - lastWake > pdMS_TO_TICKS(2)) lastWake = xTaskGetTickCount();
    }
}

void protocolTask(void*) {
    for (;;) {
        Command command{};
        if (!ble.readJsonLine(command.json, sizeof(command.json))) continue;
        command.session = ble.readSession();
        JsonDocument doc;
        if (deserializeJson(doc, command.json) || !doc.is<JsonObject>()) {
            reply("", "err", "invalid_json", command.session);
            continue;
        }
        const char* cmd = doc["cmd"] | "";
        if (BleOTA::isInProgress()) {
            ble.invalidateReceive(command.session); // Discard possible binary tail until reconnect.
            reply(cmd, "err", "ota_busy", command.session);
            continue;
        }
        const bool image = !strcmp(cmd, "recv");
        const bool standby = !strcmp(cmd, "set_standby");
        if (image || standby) {
            const int frames = doc["frames"] | 0, fps = doc["fps"] | 8;
            const size_t bytes = doc["size"].is<unsigned int>() ? doc["size"].as<unsigned int>() : 0;
            if ((image && bytes != kFrameBytes) || (standby && !validStandby(frames, fps, bytes))) {
                reply(cmd, "err", "invalid_params", command.session);
                if (image) ble.invalidateReceive(command.session); // recv has no READY handshake.
                continue;
            }
            command.data = static_cast<uint8_t*>(ps_malloc(bytes));
            if (!command.data && image) command.data = static_cast<uint8_t*>(malloc(bytes));
            if (!command.data) {
                reply(cmd, "err", "out_of_memory", command.session);
                if (image) ble.invalidateReceive(command.session);
                continue;
            }
            command.bytes = bytes;
            if (standby && !reply(cmd, "ready", nullptr, command.session)) {
                free(command.data);
                continue;
            }
            const int count = ble.readLargeData(command.data, bytes, image ? 5000 : 300000, command.session);
            if (count != static_cast<int>(bytes)) {
                free(command.data);
                ble.invalidateReceive(command.session);
                reply(cmd, "err", "recv_fail", command.session);
                continue;
            }
            // Flash writes happen here on Core 0, never in the display/input task.
            if (standby && !saveStandby(command.data, bytes, frames, fps)) {
                free(command.data);
                reply(cmd, "err", "storage_fail", command.session);
                continue;
            }
        } else if (!strcmp(cmd, "clear_standby")) {
            if (!removeStandby()) {
                reply(cmd, "err", "storage_fail", command.session);
                continue;
            }
        } else if (!strcmp(cmd, "ble_name")) {
            const String name = doc["name"] | "";
            if (!ble.saveDeviceName(name)) reply(cmd, "err", "invalid_name", command.session);
            else {
                JsonDocument response;
                response["status"] = "ok"; response["cmd"] = cmd;
                response["name"] = ble.getDeviceName(); response["apply"] = "reboot";
                char text[160];
                const size_t length = serializeJson(response, text, sizeof(text) - 2);
                text[length] = '\n'; text[length + 1] = 0;
                ble.notify(text, command.session);
                restartAt = millis() + 500;
            }
            continue;
        }
        command.timeUs = nowUs(); // Queue latency excludes radio transfer/storage time.
        // Backpressure pauses this consumer; the input and renderer keep running.
        if (xQueueSend(commandQueue, &command, pdMS_TO_TICKS(20)) != pdTRUE) {
            ++commandDrops;
            free(command.data);
            reply(cmd, "err", "busy", command.session);
        }
    }
}

void serviceTask(void*) {
    for (;;) {
        loopOTA();
        ble.service();
        const uint32_t rebootTime = restartAt.load();
        if (rebootTime && static_cast<int32_t>(millis() - rebootTime) >= 0) ESP.restart();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void createTask(TaskFunction_t function, const char* name, uint32_t stack,
                UBaseType_t priority, TaskHandle_t* handle, BaseType_t core) {
    if (xTaskCreatePinnedToCore(function, name, stack, nullptr, priority, handle, core) != pdPASS) {
        Serial.printf("Fatal: could not create %s\n", name);
        abort();
    }
}
} // namespace

bool postInput(Input input) {
    const InputEvent event{input, nowUs()};
    if (!inputQueue || xQueueSend(inputQueue, &event, 0) != pdTRUE) {
        ++inputDrops;
        return false;
    }
    return true;
}

void startRuntime() {
    inputQueue = xQueueCreate(32, sizeof(InputEvent));
    commandQueue = xQueueCreate(8, sizeof(Command));
    if (!inputQueue || !commandQueue) { Serial.println("Fatal: runtime queue allocation failed"); abort(); }
    createTask(renderTask, "pp_render", 12288, 2, &renderHandle, 1);
    createTask(inputTask, "pp_input", 3072, 4, &inputHandle, 1);
    createTask(serviceTask, "pp_service", 6144, 1, &serviceHandle, 0);
    createTask(protocolTask, "pp_protocol", 8192, 3, &protocolHandle, 0);
    xTaskNotifyGive(renderHandle);
}
} // namespace pixelpad
