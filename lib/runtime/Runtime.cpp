#include "Runtime.h"
#include "ProtocolLimits.h"
#include "StandbyStore.h"
#include "RotaryFrameCache.h"
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
struct InputEvent { Input input; uint32_t timeUs, sequence, session; };
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
std::atomic<uint32_t> inputQueueRetries{0}, inputQueuePeak{0};
uint32_t inputSequence = 0; // Input task only.
uint32_t inputTxRetries = 0, lastInputSequence = 0; // Render task only.
// Statistics below are owned exclusively by the render task.
uint32_t inputCount = 0, commandCount = 0, maxInputUs = 0, maxCommandUs = 0, maxRenderUs = 0;
enum class Mode { Image, Life, Pacman, Sand };
Mode mode = Mode::Image;
bool allWhite = false;
RotaryFrameCache rotaryCache;
uint32_t cacheEpoch = 0, displayedCacheRevision = 0, localSteps = 0;
uint32_t lastConnectionInputSequence = 0;
uint32_t attemptedInputSequence = 0, localEventSession = 0, lastLocalInputSequence = 0;
int32_t localEventRegion = -1;
bool inputAttempted = false;

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
        "\"rx_oversize\":%lu,\"rx_queue_peak\":%lu,"
        "\"heap\":%u,\"min_heap\":%u,\"stack_free\":[%u,%u,%u,%u]}\n",
        (unsigned long)inputCount, (unsigned long)commandCount,
        (unsigned long)maxInputUs, (unsigned long)maxCommandUs, (unsigned long)maxRenderUs,
        (unsigned long)ble.rxOverflows(), (unsigned long)ble.txDrops(),
        (unsigned long)(inputDrops.load() + myControl.buttonDrops()), (unsigned long)commandDrops.load(),
        (unsigned long)ble.rxOversize(), (unsigned long)ble.rxPeak(),
        ESP.getFreeHeap(), ESP.getMinFreeHeap(),
        (unsigned)uxTaskGetStackHighWaterMark(inputHandle),
        (unsigned)uxTaskGetStackHighWaterMark(renderHandle),
        (unsigned)uxTaskGetStackHighWaterMark(protocolHandle),
        (unsigned)uxTaskGetStackHighWaterMark(serviceHandle));
    ble.notify(text, session);
}

void drawFrame(const uint8_t* data, uint16_t fade) {
    appManager.stop(); mode = Mode::Image; allWhite = false;
    for (int y = 0, i = 0; y < 52; ++y)
        for (int x = 0; x < 52; ++x, i += 3)
            ledDisplay.setPixel(x, y, data[i], data[i + 1], data[i + 2]);
    ledDisplay.present(fade);
}
void presentCachedTarget() {
    if (rotaryCache.target() && displayedCacheRevision != rotaryCache.revision()) {
        drawFrame(rotaryCache.target(), rotaryCache.fade());
        displayedCacheRevision = rotaryCache.revision();
    }
}
bool cacheReply(const char* cmd, const char* status, uint32_t cacheSession,
                int32_t region, uint32_t bleSession) {
    char text[192];
    snprintf(text, sizeof(text),
        "{\"cmd\":\"%s\",\"status\":\"%s\",\"session\":%lu,\"region\":%ld}\n",
        cmd, status, (unsigned long)cacheSession, (long)region);
    return ble.notify(text, bleSession);
}

bool handleInput(const InputEvent& event) {
    // Apply an input exactly once even if its notification must retry.
    if (!inputAttempted || attemptedInputSequence != event.sequence) {
        inputAttempted = true; attemptedInputSequence = event.sequence;
        if (event.session == ble.session()) lastConnectionInputSequence = event.sequence;
        localEventSession = 0; localEventRegion = -1;
        if (event.session == ble.session() &&
            (event.input == Input::Plus || event.input == Input::Minus) &&
            rotaryCache.turn(event.input == Input::Plus ? -1 : 1, millis())) {
            localEventSession = rotaryCache.session();
            localEventRegion = rotaryCache.region();
            lastLocalInputSequence = event.sequence;
            ++localSteps;
            presentCachedTarget(); // Commit on the board BEFORE notifying phone.
        }
    }
    const char* name = "click";
    switch (event.input) {
        case Input::Click: name = "click"; break;
        case Input::DoubleClick: name = "dbl_click"; break;
        case Input::Plus: name = "color_plus"; break;
        case Input::Minus: name = "color_minus"; break;
        case Input::WhiteOn: name = "all_white_on"; break;
        case Input::WhiteOff: name = "all_white_off"; break;
    }
    // Keep the event at the runtime FIFO head until TX accepts it. Never
    // replay notifications from an earlier BLE connection into the new one.
    if (ble.connected() && event.session == ble.session()) {
        char text[128];
        const uint32_t queued = uxQueueMessagesWaiting(inputQueue);
        snprintf(text, sizeof(text),
            "{\"evt\":\"%s\",\"seq\":%lu,\"pending\":%lu,\"rotary_session\":%lu,\"rotary_region\":%ld}\n",
            name, (unsigned long)event.sequence,
            (unsigned long)(queued ? queued - 1 : 0),
            (unsigned long)localEventSession, (long)localEventRegion);
        if (!ble.notify(text, event.session, false)) {
            ++inputTxRetries;
            return false;
        }
    }
    ++inputCount;
    lastInputSequence = event.sequence;
    maxInputUs = max(maxInputUs, nowUs() - event.timeUs);
    appManager.updateActivity();
    if (event.input == Input::WhiteOn) {
        rotaryCache.pause();
        allWhite = true;
        appManager.stop();
        for (int y = 0; y < 52; ++y)
            for (int x = 0; x < 52; ++x) ledDisplay.setPixel(x, y, 255, 255, 255);
    } else if (event.input == Input::WhiteOff) {
        allWhite = false; // The App resends the previous canvas, as before.
    }
    return true;
}

void sendInputStats(uint32_t session) {
    char text[512];
    snprintf(text, sizeof(text),
        "{\"status\":\"ok\",\"cmd\":\"input_stats\",\"events\":%lu,\"last_seq\":%lu,"
        "\"pending\":%u,\"queue_peak\":%lu,\"backlog\":%lu,\"backlog_peak\":%lu,"
        "\"backlog_full\":%lu,\"queue_retries\":%lu,\"tx_retries\":%lu,"
        "\"button_drops\":%lu,\"max_us\":%lu}\n",
        (unsigned long)inputCount, (unsigned long)lastInputSequence,
        (unsigned)uxQueueMessagesWaiting(inputQueue), (unsigned long)inputQueuePeak.load(),
        (unsigned long)myControl.pendingInputs(), (unsigned long)myControl.backlogPeak(),
        (unsigned long)myControl.backlogFull(), (unsigned long)inputQueueRetries.load(),
        (unsigned long)inputTxRetries, (unsigned long)myControl.buttonDrops(),
        (unsigned long)maxInputUs);
    ble.notify(text, session);
}

void handleCommand(Command& command) {
    if (!ble.receiveHealthy(command.session) || isAnyOtaInProgress()) return;
    ++commandCount;
    maxCommandUs = max(maxCommandUs, nowUs() - command.timeUs);
    JsonDocument doc;
    if (deserializeJson(doc, command.json)) return; // Already validated by the RX task.
    const char* cmd = doc["cmd"] | "";
    if (!strcmp(cmd, "rtos_stats")) {
        sendStats(command.session);
        return;
    }
    if (!strcmp(cmd, "input_stats")) {
        sendInputStats(command.session);
        return;
    }
    if (!strcmp(cmd, "display_stats")) {
        char text[384];
        ledDisplay.writeStats(text, sizeof(text));
        ble.notify(text, command.session);
        return;
    }
    if (!strcmp(cmd, "capabilities")) {
        ble.notify("{\"cmd\":\"capabilities\",\"status\":\"ok\",\"rotary_frames_v1\":true,\"max_regions\":64}\n", command.session);
        return;
    }
    if (!strcmp(cmd, "rotary_stats")) {
        char text[256];
        snprintf(text, sizeof(text),
            "{\"cmd\":\"rotary_stats\",\"status\":\"ok\",\"session\":%lu,\"loaded\":%u,\"count\":%u,\"armed\":%s,\"region\":%ld,\"local_steps\":%lu}\n",
            (unsigned long)rotaryCache.session(), (unsigned)rotaryCache.loaded(),
            (unsigned)rotaryCache.count(), rotaryCache.armed() ? "true" : "false",
            (long)rotaryCache.region(), (unsigned long)localSteps);
        ble.notify(text, command.session); return;
    }
    const uint32_t cacheSession = doc["session"] | 0u;
    const int32_t region = doc["region"] | -1;
    if (!strcmp(cmd, "rotary_frame_begin")) {
        int32_t order[RotaryFrameCache::MaxRegions];
        JsonArray ids = doc["order"].as<JsonArray>();
        bool valid = !ids.isNull() && ids.size() > 0 && ids.size() <= RotaryFrameCache::MaxRegions;
        if (valid) for (size_t i = 0; i < ids.size(); ++i) {
            if (!ids[i].is<int32_t>() || ids[i].as<int32_t>() < 0) { valid = false; break; }
            order[i] = ids[i].as<int32_t>();
        }
        const bool ok = valid && rotaryCache.begin(cacheSession, order, ids.size(), doc["wrap"] | true);
        cacheReply(cmd, ok ? "ok" : "err", cacheSession, -1, command.session); return;
    }
    if (!strcmp(cmd, "rotary_frame_store")) {
        const bool ok = rotaryCache.store(cacheSession, region, command.data, command.bytes);
        cacheReply(cmd, ok ? "ok" : "err", cacheSession, region, command.session); return;
    }
    if (!strcmp(cmd, "rotary_frame_select")) {
        // A delayed phone command must not roll back a newer local knob step.
        const uint32_t afterInput = doc["after_input"] | 0u;
        // Do not enable a cursor based on a phone state that still lacks
        // earlier nonlocal events in this connection. Their handler retries.
        if (!rotaryCache.armed() && afterInput != lastConnectionInputSequence) {
            cacheReply(cmd, "err", cacheSession, region, command.session); return;
        }
        if (rotaryCache.armed() && cacheSession == rotaryCache.session() &&
            static_cast<int32_t>(lastLocalInputSequence - afterInput) > 0) {
            cacheReply(cmd, "ok", cacheSession, region, command.session); return;
        }
        const bool ok = rotaryCache.select(cacheSession, region, doc["code"] | false,
            millis(), constrain(doc["hold_ms"] | 600, 0, 2000), constrain(doc["fade_ms"] | 200, 0, 500));
        if (ok) { appManager.updateActivity(); presentCachedTarget(); }
        cacheReply(cmd, ok ? "ok" : "err", cacheSession, region, command.session); return;
    }
    if (!strcmp(cmd, "rotary_frame_clear")) {
        if (cacheSession == rotaryCache.session()) rotaryCache.clear();
        cacheReply(cmd, "ok", cacheSession, -1, command.session); return;
    }
    if (!strcmp(cmd, "rotary_cache_clear")) {
        rotaryCache.clear(); reply(cmd, "ok", nullptr, command.session); return;
    }
    appManager.updateActivity();
    if (strcmp(cmd, "bright")) rotaryCache.pause(); // Explicit canvas changes take ownership.
    if (!strcmp(cmd, "recv")) {
        appManager.stop();
        mode = Mode::Image;
        allWhite = false;
        for (int y = 0, i = 0; y < 52; ++y) {
            for (int x = 0; x < 52; ++x, i += 3)
                ledDisplay.setPixel(x, y, command.data[i], command.data[i + 1], command.data[i + 2]);
        }
        ledDisplay.present(constrain(doc["fade_ms"] | 0, 0, 500));
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
        if (cacheEpoch != ble.session()) {
            rotaryCache.clear(); cacheEpoch = ble.session();
            localEventSession = 0; lastLocalInputSequence = 0; lastConnectionInputSequence = 0;
        }
        InputEvent event;
        for (unsigned i = 0; i < 8 && xQueuePeek(inputQueue, &event, 0) == pdTRUE; ++i) {
            if (isAnyOtaInProgress()) ++inputDrops;
            else if (!handleInput(event)) break;
            xQueueReceive(inputQueue, &event, 0);
        }
        Command command;
        for (unsigned i = 0; i < 4 && xQueueReceive(commandQueue, &command, 0) == pdTRUE; ++i) {
            handleCommand(command);
            free(command.data);
        }
        if (!isAnyOtaInProgress() && !allWhite) {
            rotaryCache.tick(millis());
            presentCachedTarget();
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
        ledDisplay.present();
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

struct ContentLease {
    bool held=false;
    bool acquire() { held=transferGate.claim(TransferOwner::Content); return held; }
    ~ContentLease() { if (held) transferGate.release(TransferOwner::Content); }
};

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
        if (isAnyOtaInProgress()) {
            ble.invalidateReceive(command.session); // Discard possible binary tail until reconnect.
            reply(cmd, "err", "ota_busy", command.session);
            continue;
        }
        ContentLease lease;
        if ((!strcmp(cmd,"recv") || !strcmp(cmd,"set_standby") ||
             !strcmp(cmd,"clear_standby") || !strcmp(cmd,"ble_name")) && !lease.acquire()) {
            reply(cmd,"err","busy",command.session);
            ble.invalidateReceive(command.session);
            continue;
        }
        if (!strcmp(cmd, "rotary_frame_store")) {
            const uint32_t cacheSession = doc["session"] | 0u;
            const int32_t region = doc["region"] | -2;
            const size_t bytes = doc["size"].is<unsigned int>() ? doc["size"].as<unsigned int>() : 0;
            const size_t expected = kFrameBytes * (region == -1 ? 1 : 2);
            if (!cacheSession || region < -1 || bytes != expected) {
                cacheReply(cmd, "err", cacheSession, region, command.session); continue;
            }
            command.data = static_cast<uint8_t*>(ps_malloc(bytes));
            if (!command.data) { cacheReply(cmd, "err", cacheSession, region, command.session); continue; }
            command.bytes = bytes;
            if (!cacheReply(cmd, "ready", cacheSession, region, command.session)) {
                free(command.data); continue;
            }
            if (ble.readLargeData(command.data, bytes, 5000, 30000, command.session) != int(bytes)) {
                free(command.data); ble.invalidateReceive(command.session);
                cacheReply(cmd, "err", cacheSession, region, command.session); continue;
            }
            command.timeUs = nowUs();
            if (xQueueSend(commandQueue, &command, pdMS_TO_TICKS(20)) != pdTRUE) {
                ++commandDrops; free(command.data);
                cacheReply(cmd, "err", cacheSession, region, command.session);
            }
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
            const int count = ble.readLargeData(command.data, bytes,
                image ? 5000 : 300000, image ? 30000 : 300000, command.session);
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
                const uint32_t due = millis() + 500;
                restartAt = due ? due : 1;
                lease.held = false; // Retain Content ownership until reboot; block new OTA.
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
    const InputEvent event{input, nowUs(), inputSequence + 1, ble.session()};
    if (!inputQueue || xQueueSend(inputQueue, &event, 0) != pdTRUE) {
        ++inputQueueRetries;
        return false;
    }
    ++inputSequence;
    const uint32_t queued = uxQueueMessagesWaiting(inputQueue);
    if (queued > inputQueuePeak.load()) inputQueuePeak = queued;
    return true;
}

uint32_t inputSession() { return ble.session(); }

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
