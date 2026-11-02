#include "StandbyStore.h"
#include "ProtocolLimits.h"
#include <Arduino.h>
#include <LittleFS.h>

namespace pixelpad {
namespace {
bool mounted = false;
constexpr uint32_t kMagic = 0x53544259;
constexpr const char* kPath = "/standby.bin";
constexpr const char* kTempPath = "/standby.tmp";
}

void mountStandbyStore() {
    // Retain existing data if mounting fails. Provision a new filesystem
    // explicitly with `pio run -t uploadfs` instead of formatting on failure.
    mounted = LittleFS.begin(false);
    if (!mounted) Serial.println("Standby storage unavailable; display/BLE still work");
}

uint8_t* loadStandby(int& frames, int& fps) {
    if (!mounted || !LittleFS.exists(kPath)) return nullptr;
    File file = LittleFS.open(kPath, "r");
    uint32_t header[3];
    if (!file || file.read(reinterpret_cast<uint8_t*>(header), sizeof(header)) != sizeof(header) ||
        header[0] != kMagic || header[1] > kMaxStandbyFrames || header[2] > kMaxStandbyFps)
        return nullptr;
    const size_t bytes = static_cast<size_t>(header[1]) * kFrameBytes;
    if (!validStandby(header[1], header[2], bytes) || file.size() != bytes + sizeof(header))
        return nullptr;
    uint8_t* data = static_cast<uint8_t*>(ps_malloc(bytes));
    if (!data) return nullptr;
    if (file.read(data, bytes) != bytes) { free(data); return nullptr; }
    frames = header[1];
    fps = header[2];
    return data;
}

bool saveStandby(const uint8_t* data, size_t bytes, int frames, int fps) {
    if (!mounted || !data || !validStandby(frames, fps, bytes)) return false;
    File file = LittleFS.open(kTempPath, "w");
    if (!file) return false;
    const uint32_t header[] = {kMagic, static_cast<uint32_t>(frames), static_cast<uint32_t>(fps)};
    bool ok = file.write(reinterpret_cast<const uint8_t*>(header), sizeof(header)) == sizeof(header);
    for (size_t offset = 0; ok && offset < bytes; ) {
        const size_t count = min(static_cast<size_t>(4096), bytes - offset);
        ok = file.write(data + offset, count) == count;
        offset += count;
        vTaskDelay(1); // Let protocol/system/idle tasks run between flash chunks.
    }
    file.flush();
    file.close();
    if (ok) ok = LittleFS.rename(kTempPath, kPath);
    if (!ok) LittleFS.remove(kTempPath);
    return ok;
}

bool removeStandby() {
    return mounted && (!LittleFS.exists(kPath) || LittleFS.remove(kPath));
}
} // namespace pixelpad
