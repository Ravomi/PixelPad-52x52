#include "Runtime.h"
#include "ProtocolLimits.h"
#include "StandbyStore.h"
#include "RotaryFrameCache.h"
#include "ColorMenuInput.h"
#include "InputNotifications.h"
#include "Bluetooth.h"
#include "BleOTA.h"
#include "OTA.h"
#include "button.h"
#include "led.h"
#include "app.h"
#include "GameOfLife.h"
#include "Pacman.h"
#include "SandTetris.h"
#include "../ui/Tween.h"
#include <ArduinoJson.h>
#include <atomic>
#include <math.h>
#include <string.h>
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
uint32_t localEventSession = 0, lastLocalInputSequence = 0;
int32_t localEventRegion = -1;
InputNotifications notifications;
bool colorMenuWasOpen = false;
bool colorSelectorSession = false;
float colorMenuCursor = 0.0f;
float colorMenuTarget = 0.0f;
bool colorMenuCompleting = false;
bool colorMenuCompletionFinished = false;
uint32_t colorMenuCompletionStarted = 0;
bool reopenMenuAfterCacheLoad = false;
bool fireworksActive = false;
uint32_t fireworksStarted = 0;
// Shared by the render task only. Keeping the 8 KiB LED frame off the task
// stack prevents a long-press menu draw from overflowing the render task.
uint8_t* effectFrame = nullptr; // Allocated in PSRAM before render task starts.

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
void menuPixel(uint8_t* frame, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || x >= 52 || y < 0 || y >= 52) return;
    const size_t at = (static_cast<size_t>(y) * 52u + static_cast<size_t>(x)) * 3u;
    frame[at] = r; frame[at + 1] = g; frame[at + 2] = b;
}
void menuRoundRect(uint8_t* frame, int x, int y, int w, int h,
                   uint8_t fr, uint8_t fg, uint8_t fb,
                   uint8_t er, uint8_t eg, uint8_t eb) {
    constexpr int radius = 6;
    for (int py = 0; py < h; ++py) for (int px = 0; px < w; ++px) {
        const bool corner = (px < radius || px >= w - radius) &&
                            (py < radius || py >= h - radius);
        int dx = 0, dy = 0;
        if (px < radius) dx = radius - 1 - px;
        else if (px >= w - radius) dx = px - (w - radius);
        if (py < radius) dy = radius - 1 - py;
        else if (py >= h - radius) dy = py - (h - radius);
        const int distanceSquared = dx * dx + dy * dy;
        if (corner && distanceSquared > radius * radius) continue;
        const bool edge = px == 0 || px == w - 1 || py == 0 || py == h - 1 ||
                          (corner && distanceSquared >= (radius - 1) * (radius - 1));
        menuPixel(frame, x + px, y + py, edge ? er : fr, edge ? eg : fg, edge ? eb : fb);
    }
}
void menuGlyph(uint8_t* frame, int x, int y, char c, uint8_t r, uint8_t g, uint8_t b) {
    static const uint8_t glyphs[][5] = {
        {14,17,17,17,14},{4,12,4,4,14},{14,1,14,16,15},{14,1,6,1,14},
        {2,6,10,31,2},{31,16,30,1,30},{14,16,30,17,14},{31,1,2,4,4},
        {14,17,14,17,14},{14,17,15,1,14},
        {14,17,31,17,17},{30,17,30,17,30},{14,17,16,17,14},
        {30,17,17,17,30},{31,16,30,16,31},{31,16,30,16,16},
        {14,17,23,16,15},{17,17,31,17,17},{14,4,4,4,14},
        {7,2,2,18,12},{17,18,28,18,17},{16,16,16,16,31},
        {17,27,21,17,17},{17,25,21,19,17},{14,17,17,17,14},
        {30,17,30,16,16},{14,17,17,21,15},{30,17,30,20,17},
        {15,16,14,1,30},{31,4,4,4,4},{17,17,17,17,14},
        {17,17,17,10,4},{17,17,21,21,10},{17,10,4,10,17},
        {17,10,4,4,4},{31,2,4,8,31},
        {0,0,0,0,0}
    };
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    const int index = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'Z' ? 10 + c - 'A' : 36;
    for (int row = 0; row < 5; ++row) for (int col = 0; col < 5; ++col)
        if (glyphs[index][row] & (1u << (4 - col))) menuPixel(frame, x + col, y + row, r, g, b);
}
void menuText(uint8_t* frame, int x, int y, int w, const char* text,
              uint8_t r, uint8_t g, uint8_t b) {
    if (!text) return;
    size_t length = strlen(text); if (length > 8) length = 8;
    const int start = x + (w - static_cast<int>(length) * 6 + 1) / 2;
    for (size_t i = 0; i < length; ++i)
        menuGlyph(frame, start + static_cast<int>(i) * 6, y, text[i], r, g, b);
}
void drawColorMenuFrame(uint32_t now, bool fadeIn = false) {
    static uint32_t lastSubmitted = 0;
    const size_t count = rotaryCache.count();
    if (!count) return;
    if (lastSubmitted != 0 && now - lastSubmitted < 4) return;
    const float target = colorMenuTarget;
    if (!colorMenuWasOpen) {
        colorMenuCursor = static_cast<float>(rotaryCache.cursorIndex());
        colorMenuTarget = colorMenuCursor;
        colorMenuWasOpen = true;
    } else {
        const float distance = fabsf(colorMenuTarget - colorMenuCursor);
        const float speed = distance > 1.5f ? 94.0f : 88.0f;
        pixelpad::FollowTween::step(colorMenuCursor, target, speed, 0.01f);
    }
    const float animated = colorMenuCursor;
    float completion = 0.0f;
    if (colorMenuCompleting || colorMenuCompletionFinished) {
        completion = fminf(1.0f, static_cast<float>(now - colorMenuCompletionStarted) / 220.0f);
        completion = completion * completion * (3.0f - 2.0f * completion);
        if (completion >= 1.0f) {
            colorMenuCompleting = false;
            colorMenuCompletionFinished = true;
        }
    }
    memset(effectFrame, 0, kFrameBytes);
    uint8_t* frame = effectFrame;
    for (size_t i = 0; i < count; ++i) {
        float delta = static_cast<float>(i) - animated;
        if (count > 1) {
            while (delta > static_cast<float>(count) / 2.0f) delta -= count;
            while (delta < -static_cast<float>(count) / 2.0f) delta += count;
        }
        // Keep only the selected color and its two neighbours visible. This
        // leaves enough pixels for complete labels such as F14 without making
        // the 52x52 menu feel crowded.
        const bool selected = fabsf(delta) < 0.34f;
        if (colorMenuCompletionFinished && selected) continue;
        if (colorMenuCompleting && selected) {
            // The selected card exits to the right while the lower card takes
            // its orbital slot, matching the completion action in the app.
        } else if (colorMenuCompleting || colorMenuCompletionFinished) {
            if (delta > 0.5f && delta < 1.5f) delta -= completion;
        }
        if (fabsf(delta) > 1.25f && !(colorMenuCompleting && selected)) continue;
        char label[16]; uint8_t color[3];
        if (!rotaryCache.menuItem(i, label, sizeof(label), color)) continue;
        const int labelWidth = static_cast<int>(strlen(label)) * 6 + 2;
        float focus = fmaxf(0.0f, 1.0f - fabsf(delta));
        focus = focus * focus * (3.0f - 2.0f * focus);
        const int baseWidth = constrain(labelWidth + 2, 18, 24);
        const int w = static_cast<int>(baseWidth + (30 - baseWidth) * focus + 0.5f);
        const int h = static_cast<int>(13.0f + 7.0f * focus + 0.5f);
        // The physical encoder is at the upper-left of the panel. Keep the
        // three cards on a small orbit around that point instead of placing
        // the menu around the LED matrix center.
        constexpr float pivotX = 5.0f;
        constexpr float pivotY = 5.0f;
        constexpr float orbitRadius = 28.0f;
        constexpr float slotAngle = 0.62f;
        const float angle = 0.72f + delta * slotAngle;
        int cx = static_cast<int>(pivotX + cosf(angle) * orbitRadius);
        const int cy = static_cast<int>(pivotY + sinf(angle) * orbitRadius);
        if (selected && (colorMenuCompleting || colorMenuCompletionFinished)) {
            cx += static_cast<int>(32.0f * completion);
        }
        const int x = cx - w / 2, y = cy - h / 2;
        const bool dark = (299 * color[0] + 587 * color[1] + 114 * color[2]) < 110000;
        const uint8_t ink = dark ? 255 : 0;
        menuRoundRect(frame, x, y, w, h, color[0], color[1], color[2], ink, ink, ink);
        menuText(frame, x, y + (h - 5) / 2, w, label, ink, ink, ink);
    }
    ledDisplay.submitFrame(frame, fadeIn ? 180 : 0);
    lastSubmitted = now;
}
void drawFireworksFrame(uint32_t now) {
    constexpr uint32_t durationMs = 1900;
    const uint32_t elapsed = now - fireworksStarted;
    memset(effectFrame, 0, kFrameBytes);
    uint8_t* frame = effectFrame;
    if (elapsed >= durationMs) {
        fireworksActive = false;
        ledDisplay.submitFrame(frame);
        return;
    }
    static const int centers[][2] = {{15, 17}, {37, 14}, {26, 34}};
    static const uint8_t hues[] = {24, 150, 224, 42, 190, 82};
    for (size_t burst = 0; burst < 3; ++burst) {
        const uint32_t birth = static_cast<uint32_t>(burst) * 300u;
        if (elapsed < birth) continue;
        const float age = fminf(1.0f, static_cast<float>(elapsed - birth) / 1250.0f);
        const float radius = 1.0f + age * 19.0f;
        const float brightness = (1.0f - age) * (1.0f - age);
        for (int spark = 0; spark < 12; ++spark) {
            const float angle = static_cast<float>(spark) * 0.5235988f + burst * 0.31f;
            const int x = centers[burst][0] + static_cast<int>(cosf(angle) * radius);
            const int y = centers[burst][1] + static_cast<int>(sinf(angle) * radius);
            const uint8_t hue = hues[(burst * 2 + spark) % (sizeof(hues) / sizeof(hues[0]))];
            const uint8_t r = static_cast<uint8_t>(brightness * (hue < 80 || hue > 210 ? 255 : 80));
            const uint8_t g = static_cast<uint8_t>(brightness * (hue > 35 && hue < 185 ? 230 : 70));
            const uint8_t b = static_cast<uint8_t>(brightness * (hue > 120 ? 255 : 70));
            menuPixel(frame, x, y, r, g, b);
            menuPixel(frame, x - 1, y, r / 2, g / 2, b / 2);
            menuPixel(frame, x, y - 1, r / 2, g / 2, b / 2);
        }
        const float flash = 1.0f - fminf(1.0f, static_cast<float>(elapsed - birth) / 120.0f);
        if (flash > 0.0f) {
            const uint8_t white = static_cast<uint8_t>(flash * 255.0f);
            menuPixel(frame, centers[burst][0], centers[burst][1], white, white, white);
        }
    }
    ledDisplay.submitFrame(frame);
}
void presentCachedTarget() {
    if (rotaryCache.menuOpen()) {
        drawColorMenuFrame(millis());
        return;
    }
    colorMenuWasOpen = false;
    colorMenuCompleting = false;
    colorMenuCompletionFinished = false;
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

void handleInput(const InputEvent& event) {
    // Apply locally without waiting for the phone or its notification queue.
    {
        if (event.session == ble.session()) lastConnectionInputSequence = event.sequence;
        localEventSession = 0; localEventRegion = -1;
        const ColorMenuAction action = handleColorMenuInput(rotaryCache, event.input, millis(), colorSelectorSession);
        if (action == ColorMenuAction::Step || action == ColorMenuAction::Boundary) {
            localEventSession = rotaryCache.session(); localEventRegion = rotaryCache.region();
            lastLocalInputSequence = event.sequence; ++localSteps;
            colorMenuCompleting = colorMenuCompletionFinished = false;
            if (!colorMenuWasOpen) colorMenuTarget = static_cast<float>(rotaryCache.cursorIndex());
            else if (action == ColorMenuAction::Step) colorMenuTarget += event.input == Input::Plus ? -1.0f : 1.0f;
            presentCachedTarget();
        } else if (action == ColorMenuAction::Open) {
            colorSelectorSession = true;
            colorMenuWasOpen = false;
            colorMenuCompleting = colorMenuCompletionFinished = false;
            localEventSession = rotaryCache.session(); localEventRegion = rotaryCache.region();
            appManager.stop(); mode = Mode::Image; allWhite = false;
            drawColorMenuFrame(millis(), true);
        } else if (action == ColorMenuAction::Complete) {
            colorMenuCompleting = true; colorMenuCompletionFinished = false;
            colorMenuCompletionStarted = millis();
            localEventSession = rotaryCache.session(); localEventRegion = rotaryCache.region();
            presentCachedTarget();
        } else if (action == ColorMenuAction::Confirm) {
            colorSelectorSession = false;
            localEventSession = rotaryCache.session(); localEventRegion = rotaryCache.region();
            lastLocalInputSequence = event.sequence;
            presentCachedTarget();
        } else if (action == ColorMenuAction::Unavailable) {
            // No fabricated palette: explain that the phone must preload the artwork.
            appManager.stop(); mode = Mode::Image; allWhite = false;
            memset(effectFrame, 0, kFrameBytes);
            menuText(effectFrame, 0, 18, 52, "OPEN APP", 100, 100, 100);
            menuText(effectFrame, 0, 29, 52, "LOAD ART", 70, 70, 70);
            ledDisplay.submitFrame(effectFrame, 180);
        }
    }
    if (ble.connected() && event.session == ble.session()) {
        notifications.push({event.input, event.sequence, event.session,
                            localEventSession, localEventRegion});
    }
    ++inputCount;
    lastInputSequence = event.sequence;
    maxInputUs = max(maxInputUs, nowUs() - event.timeUs);
    appManager.updateActivity();
}

// Called only AFTER ledDisplay.present(): BLE congestion cannot delay local
// input, and the phone cannot receive an event before its frame is submitted.
void flushInputNotifications() {
    for (unsigned i = 0; i < 8 && notifications.size(); ++i) {
        const auto& event = notifications.front();
        if (!ble.connected() || event.session != ble.session()) {
            notifications.pop();
            continue;
        }
        const char* name = "click";
        switch (event.input) {
            case Input::Click: name = "click"; break;
            case Input::DoubleClick: name = "dbl_click"; break;
            case Input::Plus: name = "color_plus"; break;
            case Input::Minus: name = "color_minus"; break;
            case Input::WhiteOn: name = "menu_open"; break;
            case Input::WhiteOff: name = "menu_close"; break;
        }
        char text[160];
        snprintf(text, sizeof(text),
            "{\"evt\":\"%s\",\"seq\":%lu,\"pending\":%lu,\"rotary_session\":%lu,\"rotary_region\":%ld}\n",
            name, (unsigned long)event.sequence,
            (unsigned long)(notifications.size() - 1 + uxQueueMessagesWaiting(inputQueue)),
            (unsigned long)event.rotarySession, (long)event.region);
        if (!ble.notify(text, event.session, false)) {
            ++inputTxRetries;
            break;
        }
        notifications.pop();
    }
}

void sendInputStats(uint32_t session) {
    char text[512];
    snprintf(text, sizeof(text),
        "{\"status\":\"ok\",\"cmd\":\"input_stats\",\"events\":%lu,\"last_seq\":%lu,"
        "\"pending\":%u,\"queue_peak\":%lu,\"backlog\":%lu,\"backlog_peak\":%lu,"
        "\"backlog_full\":%lu,\"queue_retries\":%lu,\"tx_retries\":%lu,"
        "\"button_drops\":%lu,\"max_us\":%lu,\"notify_pending\":%lu,\"notify_drops\":%lu}\n",
        (unsigned long)inputCount, (unsigned long)lastInputSequence,
        (unsigned)uxQueueMessagesWaiting(inputQueue), (unsigned long)inputQueuePeak.load(),
        (unsigned long)myControl.pendingInputs(), (unsigned long)myControl.backlogPeak(),
        (unsigned long)myControl.backlogFull(), (unsigned long)inputQueueRetries.load(),
        (unsigned long)inputTxRetries, (unsigned long)myControl.buttonDrops(),
        (unsigned long)maxInputUs, (unsigned long)notifications.size(),
        (unsigned long)notifications.drops());
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
        char text[512];
        ledDisplay.writeStats(text, sizeof(text));
        ble.notify(text, command.session);
        return;
    }
    if (!strcmp(cmd, "capabilities")) {
        ble.notify("{\"cmd\":\"capabilities\",\"status\":\"ok\",\"rotary_frames_v1\":true,\"max_regions\":64,\"rotary_menu_v1\":true}\n", command.session);
        return;
    }
    if (!strcmp(cmd, "rotary_stats")) {
        char text[256];
        snprintf(text, sizeof(text),
            "{\"cmd\":\"rotary_stats\",\"status\":\"ok\",\"session\":%lu,\"loaded\":%u,\"count\":%u,\"armed\":%s,\"region\":%ld,\"local_steps\":%lu,\"menu_open\":%s}\n",
            (unsigned long)rotaryCache.session(), (unsigned)rotaryCache.loaded(),
            (unsigned)rotaryCache.count(), rotaryCache.armed() ? "true" : "false",
            (long)rotaryCache.region(), (unsigned long)localSteps,
            rotaryCache.menuOpen() ? "true" : "false");
        ble.notify(text, command.session); return;
    }
    const uint32_t cacheSession = doc["session"] | 0u;
    const int32_t region = doc["region"] | -1;
    if (!strcmp(cmd, "rotary_frame_begin")) {
        const bool keepMenuOpen = reopenMenuAfterCacheLoad || rotaryCache.menuOpen();
        int32_t order[RotaryFrameCache::MaxRegions];
        const char* labels[RotaryFrameCache::MaxRegions] = {};
        uint8_t colors[RotaryFrameCache::MaxRegions][3] = {};
        JsonArray ids = doc["order"].as<JsonArray>();
        bool valid = !ids.isNull() && ids.size() > 0 && ids.size() <= RotaryFrameCache::MaxRegions;
        if (valid) for (size_t i = 0; i < ids.size(); ++i) {
            if (!ids[i].is<int32_t>() || ids[i].as<int32_t>() < 0) { valid = false; break; }
            order[i] = ids[i].as<int32_t>();
            labels[i] = doc["labels"][i] | "";
            JsonArray color = doc["colors"][i].as<JsonArray>();
            if (!color.isNull() && color.size() >= 3) {
                colors[i][0] = constrain(color[0] | 0, 0, 255);
                colors[i][1] = constrain(color[1] | 0, 0, 255);
                colors[i][2] = constrain(color[2] | 0, 0, 255);
            } else {
                colors[i][0] = colors[i][1] = colors[i][2] = 40;
            }
        }
        const bool ok = valid && rotaryCache.begin(cacheSession, order, ids.size(), doc["wrap"] | true, labels, colors);
        reopenMenuAfterCacheLoad = ok && keepMenuOpen;
        cacheReply(cmd, ok ? "ok" : "err", cacheSession, -1, command.session); return;
    }
    if (!strcmp(cmd, "rotary_frame_store")) {
        const bool ok = rotaryCache.store(cacheSession, region, command.data, command.bytes);
        if (ok && reopenMenuAfterCacheLoad && rotaryCache.ready()) {
            reopenMenuAfterCacheLoad = false;
            colorMenuWasOpen = false;
            colorMenuCompleting = false;
            colorMenuCompletionFinished = false;
            rotaryCache.openMenu(millis());
            colorSelectorSession = true;
            presentCachedTarget();
        }
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
        reopenMenuAfterCacheLoad = reopenMenuAfterCacheLoad ||
            rotaryCache.menuOpen() || colorSelectorSession;
        // Keep the current local cache alive while the phone rebuilds the
        // order. The following begin command replaces it atomically; this
        // avoids a dead gap after completing one card.
        if (cacheSession == rotaryCache.session() && !colorSelectorSession)
            rotaryCache.clear();
        cacheReply(cmd, "ok", cacheSession, -1, command.session); return;
    }
    if (!strcmp(cmd, "rotary_frame_complete")) {
        if (cacheSession == rotaryCache.session() &&
            (reopenMenuAfterCacheLoad || rotaryCache.menuOpen())) {
            reopenMenuAfterCacheLoad = false;
            colorSelectorSession = false;
            fireworksActive = true;
            fireworksStarted = millis();
            rotaryCache.clear();
            colorMenuWasOpen = false;
            colorMenuCompleting = false;
            colorMenuCompletionFinished = false;
        }
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
            notifications.clear();
            colorMenuWasOpen = colorSelectorSession = false;
            colorMenuCompleting = colorMenuCompletionFinished = false;
            reopenMenuAfterCacheLoad = fireworksActive = false;
            localEventSession = 0; lastLocalInputSequence = 0; lastConnectionInputSequence = 0;
        }
        InputEvent event;
        for (unsigned i = 0; i < 8 && xQueuePeek(inputQueue, &event, 0) == pdTRUE; ++i) {
            if (isAnyOtaInProgress()) ++inputDrops;
            else handleInput(event);
            xQueueReceive(inputQueue, &event, 0);
        }
        Command command;
        for (unsigned i = 0; i < 4 && xQueueReceive(commandQueue, &command, 0) == pdTRUE; ++i) {
            handleCommand(command);
            free(command.data);
        }
        if (!isAnyOtaInProgress() && !allWhite) {
            rotaryCache.tick(millis());
            if (fireworksActive) drawFireworksFrame(millis());
            else presentCachedTarget();
            if (rotaryCache.menuOpen() || fireworksActive) appManager.updateActivity();
            else appManager.loop();
            if (!rotaryCache.menuOpen() && !fireworksActive && !appManager.isAnimating() && !appManager.isLogoState()) {
                switch (mode) {
                    case Mode::Image: break; // HUB75 DMA already refreshes continuously.
                    case Mode::Life: lifeGame.loop(); break;
                    case Mode::Pacman: pacmanGame.loop(); break;
                    case Mode::Sand: sandGame.loop(); break;
                }
            }
        }
        if (ledDisplay.present()) flushInputNotifications();
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
             !strcmp(cmd,"clear_standby") || !strcmp(cmd,"ble_name") ||
             !strcmp(cmd,"rotary_frame_store")) && !lease.acquire()) {
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
    effectFrame = static_cast<uint8_t*>(ps_malloc(kFrameBytes));
    if (!effectFrame) { Serial.println("Fatal: menu frame allocation failed"); abort(); }
    inputQueue = xQueueCreate(32, sizeof(InputEvent));
    commandQueue = xQueueCreate(4, sizeof(Command)); // Bound RAM with palette metadata JSON.
    if (!inputQueue || !commandQueue) { Serial.println("Fatal: runtime queue allocation failed"); abort(); }
    createTask(renderTask, "pp_render", 12288, 2, &renderHandle, 1);
    createTask(inputTask, "pp_input", 3072, 4, &inputHandle, 1);
    createTask(serviceTask, "pp_service", 6144, 1, &serviceHandle, 0);
    createTask(protocolTask, "pp_protocol", 8192, 3, &protocolHandle, 0);
    xTaskNotifyGive(renderHandle);
}
} // namespace pixelpad
