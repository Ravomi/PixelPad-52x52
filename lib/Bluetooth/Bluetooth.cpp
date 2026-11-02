// bluetooth.cpp
#include "Bluetooth.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h>
#include "BleOTA.h"
#include "TransferGate.h"
#include "ReceiveDeadline.h"
#include <stdio.h>
#include <string>
#include <atomic>
#include <freertos/queue.h>

// Nordic UART Service UUIDs
#define SERVICE_UUID        "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"//这个是手机发送数据的特征值不能改

// PixelPad-owned scan identity. Unlike NUS and Aliyun OTA UUIDs, this custom
// service UUID is reserved for PixelPad and is always placed in the primary
// advertising packet so the App can identify the device before connecting.
#define PIXELPAD_IDENTITY_SERVICE_UUID "7DA35D3C-AD91-41BA-8A6B-D375CC1116F4"
#define PIXELPAD_IDENTITY_CHAR_UUID    "7DA35D3C-AD91-41BA-8A6B-D375CC1116F5"
#define PIXELPAD_CAPABILITIES_CHAR_UUID "7DA35D3C-AD91-41BA-8A6B-D375CC1116F6"

BLEServer* pServer = nullptr;
BLEService* pService = nullptr;
BLECharacteristic* pTxCharacteristic = nullptr;
BLECharacteristic* pRxCharacteristic = nullptr;
BLEService* pIdentityService = nullptr;
BLECharacteristic* pIdentityCharacteristic = nullptr;

// The BLE host is the only RX producer; the protocol task is the only reader.
// Tag packets by connection epoch: old bytes cannot leak into a new transfer.
static QueueHandle_t rxQueue = nullptr;
static QueueHandle_t txQueue = nullptr;
static std::atomic<bool> clientConnected{false};
static std::atomic<uint32_t> connectionSession{0};
static std::atomic<uint32_t> brokenSession{0};
static std::atomic<uint32_t> rxOverflowCount{0};
static std::atomic<uint32_t> rxOversizeCount{0}, rxQueuePeak{0};
static std::atomic<uint32_t> txDropCount{0};
static std::atomic<bool> restartAdvertising{false};
static std::atomic<uint32_t> advertisingAt{0};
struct TxPacket { uint32_t session; uint16_t size; char text[512]; };
static Preferences blePrefs;
static String bleDeviceName = "PixelPad";

#include "esp_mac.h"

static String makeDefaultDeviceName() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    char name[18];
    snprintf(name, sizeof(name), "PixelPad-%02X%02X", mac[4], mac[5]);
    return String(name);
}

class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer*) override {
        ++connectionSession;
        clientConnected = true;
        restartAdvertising = false;
    }
    void onConnect(BLEServer* server, esp_ble_gatts_cb_param_t* param) override {
        // HarmonyOS may keep MTU 23 and does not request Android's high
        // connection priority. Ask from the peripheral as well: 7.5-15 ms,
        // no skipped events, 4 s supervision timeout. The central decides.
        server->updateConnParams(param->connect.remote_bda, 6, 12, 0, 400);
    }
    void onDisconnect(BLEServer*) override {
        clientConnected = false;
        ++connectionSession;
        BleOTA::abort();
        advertisingAt = millis() + 500;
        restartAdvertising = true; // No delay inside the Bluetooth host task.
    }
};

class RxCallback : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* characteristic) override {
        const std::string value = characteristic->getValue();
        const uint32_t epoch = connectionSession.load();
        if (value.empty() || !clientConnected || brokenSession == epoch) return;
        Bluetooth::Packet packet{};
        packet.session = epoch;
        if (value.size() > sizeof(packet.bytes)) {
            brokenSession = epoch;
            ++rxOverflowCount;
            ++rxOversizeCount;
            return;
        }
        packet.size = value.size();
        memcpy(packet.bytes, value.data(), value.size());
        if (xQueueSend(rxQueue, &packet, 0) != pdTRUE) {
            brokenSession = epoch;
            ++rxOverflowCount;
        } else {
            const uint32_t queued = uxQueueMessagesWaiting(rxQueue);
            if (queued > rxQueuePeak.load()) rxQueuePeak = queued;
        }
    }
};

String Bluetooth::getDeviceName() const {
    return bleDeviceName;
}

bool Bluetooth::saveDeviceName(const String& name) {
    String trimmed = name;
    trimmed.trim();
    
    if (trimmed.length() == 0) {
        blePrefs.begin("ble_cfg", false);
        blePrefs.remove("name");
        blePrefs.end();
        bleDeviceName = makeDefaultDeviceName();
        return true;
    }
    
    if (trimmed.length() > 20) {
        return false;
    }
    
    for (size_t i = 0; i < trimmed.length(); i++) {
        const char c = trimmed.charAt(i);
        const bool ok =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_' ||
            c == '-' ||
            c == ' ';
        if (!ok) {
            return false;
        }
    }

    blePrefs.begin("ble_cfg", false);
    const bool stored = blePrefs.putString("name", trimmed) > 0;
    blePrefs.end();
    if (stored) {
        bleDeviceName = trimmed;
    }
    return stored;
}

bool Bluetooth::init() {
    if (initialized_) return true;
    // A whole 8112-byte image at ATT payload 244 needs 34 packets. A host
    // burst can arrive before this Core-0 consumer is scheduled by the BT host.
    rxQueue = xQueueCreate(64, sizeof(Packet));
    txQueue = xQueueCreate(16, sizeof(TxPacket));
    if (!rxQueue || !txQueue) return false;

    Serial.println("正在初始化 BLE UART 服务...");

    const String defaultName = makeDefaultDeviceName();
    bleDeviceName = defaultName;

    blePrefs.begin("ble_cfg", true);
    String storedName = blePrefs.getString("name", defaultName);
    blePrefs.end();
    storedName.trim();
    if (storedName.length() > 0 && storedName.length() <= 20) {
        bleDeviceName = storedName;
    }

    BLEDevice::init(bleDeviceName.c_str());//这个是蓝牙的名字
    BLEDevice::setMTU(517);
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    pIdentityService = pServer->createService(PIXELPAD_IDENTITY_SERVICE_UUID);
    pIdentityCharacteristic = pIdentityService->createCharacteristic(
        PIXELPAD_IDENTITY_CHAR_UUID,
        BLECharacteristic::PROPERTY_READ
    );
    // "PPAD", identity schema v1, hardware model v1.
    uint8_t identityValue[] = {'P', 'P', 'A', 'D', 0x01, 0x01};
    pIdentityCharacteristic->setValue(identityValue, sizeof(identityValue));
    BLECharacteristic* capabilities = pIdentityService->createCharacteristic(
        PIXELPAD_CAPABILITIES_CHAR_UUID, BLECharacteristic::PROPERTY_READ);
    capabilities->setValue("{\"model\":1,\"width\":52,\"height\":52,\"driver\":\"fm6124\",\"frameFade\":1,\"streamRecv\":1,\"rotaryMenu\":1}");
    pIdentityService->start();

    pService = pServer->createService(SERVICE_UUID);

    // TX: 手机通过 Notify 读取
    pTxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_TX,
        BLECharacteristic::PROPERTY_NOTIFY
    );
    pTxCharacteristic->addDescriptor(new BLE2902());

    // RX: 手机写入
    // pRxCharacteristic = pService->createCharacteristic(
    //     CHARACTERISTIC_UUID_RX,
    //     BLECharacteristic::PROPERTY_WRITE
    // );
    pRxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_RX,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR // ✅ 加上这一项
    );
    pRxCharacteristic->setCallbacks(new RxCallback());

    pService->start();
    BleOTA::begin(pServer);

    BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
    // A legacy advertising packet cannot reliably hold three 128-bit service
    // UUIDs plus the device name. Keep the PixelPad identity UUID in the
    // primary packet and move the user-visible name to the scan response.
    BLEAdvertisementData advertisementData;
    advertisementData.setFlags(0x06); // General discoverable, BR/EDR unsupported.
    advertisementData.setCompleteServices(
        BLEUUID(PIXELPAD_IDENTITY_SERVICE_UUID)
    );
    pAdvertising->setAdvertisementData(advertisementData);

    BLEAdvertisementData scanResponseData;
    scanResponseData.setName(std::string(bleDeviceName.c_str()));
    pAdvertising->setScanResponseData(scanResponseData);
    pAdvertising->setScanResponse(true);
    pAdvertising->start();

    Serial.println("✅ BLE 服务启动成功！");
    Serial.printf("📱 请用 nRF Connect 连接设备 '%s'\n", bleDeviceName.c_str());
    Serial.println("   → 启用 TX 特征值的 Notify");
    Serial.println("   → 向 RX 特征值发送数据（带 \\n 结尾）");

    initialized_ = true;
    return true;
}



bool Bluetooth::connected() const { return clientConnected.load(); }
uint32_t Bluetooth::session() const { return connectionSession.load(); }
uint32_t Bluetooth::rxOverflows() const { return rxOverflowCount.load(); }
uint32_t Bluetooth::rxOversize() const { return rxOversizeCount.load(); }
uint32_t Bluetooth::rxPeak() const { return rxQueuePeak.load(); }
uint32_t Bluetooth::txDrops() const { return txDropCount.load(); }
bool Bluetooth::receiveHealthy(uint32_t epoch) const {
    return connected() && session() == epoch && brokenSession != epoch;
}
void Bluetooth::invalidateReceive(uint32_t epoch) {
    // An old timed-out consumer must not clear a newer connection's RX fault.
    uint32_t previous = brokenSession.load();
    while (previous < epoch && !brokenSession.compare_exchange_weak(previous, epoch)) {}
}

int Bluetooth::readBytes(uint8_t* output, size_t capacity, uint32_t epoch, uint32_t waitMs) {
    if (!receiveHealthy(epoch)) return -1;
    if (packetOffset_ >= packet_.size || packet_.session != epoch) {
        packetOffset_ = 0;
        packet_.size = 0;
        if (xQueueReceive(rxQueue, &packet_, pdMS_TO_TICKS(waitMs)) != pdTRUE) return 0;
        if (packet_.session != epoch) { packet_.size = 0; return 0; }
    }
    if (!receiveHealthy(epoch)) return -1;
    const size_t count = min(capacity, static_cast<size_t>(packet_.size) - packetOffset_);
    memcpy(output, packet_.bytes + packetOffset_, count);
    packetOffset_ += count;
    return count;
}

bool Bluetooth::readJsonLine(char* output, size_t outputSize) {
    const uint32_t epoch = session();
    if (epoch != readSession_) {
        line_.reset();
        packet_.size = 0;
        packetOffset_ = 0;
        readSession_ = epoch;
    }
    if (!receiveHealthy(epoch)) {
        // Only this consumer drains the RX queue; no cross-core reset races.
        line_.reset();
        while (xQueueReceive(rxQueue, &packet_, 0) == pdTRUE) {
            if (packet_.session != epoch && receiveHealthy(packet_.session)) {
                readSession_ = packet_.session;
                packetOffset_ = 0;
                return false; // Preserve the first packet of a reconnecting client.
            }
        }
        packet_.size = 0;
        vTaskDelay(pdMS_TO_TICKS(5));
        return false;
    }
    // A work budget also protects Core 0 under a continuous malformed stream.
    for (size_t i = 0; i < pixelpad::kJsonBytes; ++i) {
        uint8_t byte;
        if (readBytes(&byte, 1, epoch, i == 0 ? 10 : 0) <= 0) return false;
        const auto result = line_.push(byte, output, outputSize);
        if (result == pixelpad::JsonLine::Result::Complete) return true;
        if (result == pixelpad::JsonLine::Result::TooLong) {
            notify("{\"status\":\"err\",\"reason\":\"line_too_long\"}\n", epoch);
            return false;
        }
    }
    vTaskDelay(1);
    return false;
}

int Bluetooth::readLargeData(uint8_t* buffer, int size, uint32_t idleTimeoutMs,
                             uint32_t totalTimeoutMs, uint32_t epoch) {
    if (!buffer || size <= 0) return -1;
    int received = 0;
    pixelpad::ReceiveDeadline deadline(millis(), idleTimeoutMs, totalTimeoutMs);
    while (received < size) {
        if (!receiveHealthy(epoch) || isAnyOtaInProgress()) return -1;
        const uint32_t remaining = deadline.remainingMs(millis());
        if (!remaining) return received;
        const int count = readBytes(buffer + received, size - received, epoch,
                                    min(static_cast<uint32_t>(10), remaining));
        if (count < 0) return -1;
        received += count;
        if (count) deadline.madeProgress(millis());
    }
    return receiveHealthy(epoch) ? received : -1;
}

bool Bluetooth::notify(const char* text, uint32_t epoch, bool countDrop) {
    if (!epoch) epoch = session();
    if (!connected() || session() != epoch || !text) return false;
    // Retriable input traffic leaves room for frame ACKs/control replies.
    if (!countDrop && uxQueueSpacesAvailable(txQueue) <= 4) return false;
    TxPacket packet{};
    packet.session = epoch;
    const size_t length = strlen(text);
    if (length > sizeof(packet.text)) { ++txDropCount; return false; }
    packet.size = length;
    memcpy(packet.text, text, length);
    if (xQueueSend(txQueue, &packet, 0) != pdTRUE) {
        if (countDrop) ++txDropCount;
        return false;
    }
    return true;
}

void Bluetooth::service() {
    if (restartAdvertising && !connected() &&
        static_cast<int32_t>(millis() - advertisingAt.load()) >= 0) {
        restartAdvertising = false;
        pServer->startAdvertising();
    }
    static uint32_t reportedBrokenSession = 0;
    static uint32_t faultSession = 0, faultDisconnectAt = 0;
    const uint32_t epoch = session();
    if (connected() && brokenSession == epoch && reportedBrokenSession != epoch) {
        if (notify("{\"status\":\"err\",\"reason\":\"rx_desync_reconnect\"}\n", epoch)) {
            reportedBrokenSession = epoch;
            faultSession = epoch;
            faultDisconnectAt = millis() + 250;
        }
    }
    // Do not leave the App showing a live connection whose commands are all
    // rejected. Allow the queued error replies to drain, then require a fresh
    // connection epoch; late binary bytes can never be parsed as commands.
    if (faultSession && faultSession == epoch && connected() &&
        static_cast<int32_t>(millis() - faultDisconnectAt) >= 0) {
        faultSession = 0;
        pServer->disconnect(pServer->getConnId());
        return;
    }
    TxPacket packet;
    // Limit work so ElegantOTA maintenance still runs under a busy TX stream.
    if (xQueueReceive(txQueue, &packet, 0) != pdTRUE) return;
    if (!connected() || packet.session != session()) return;
    const uint16_t mtu = pServer->getPeerMTU(pServer->getConnId());
    const size_t payload = mtu > 3 ? mtu - 3 : 20;
    for (size_t offset = 0; offset < packet.size; offset += payload) {
        if (!connected() || packet.session != session()) return;
        const size_t count = min(payload, static_cast<size_t>(packet.size) - offset);
        pTxCharacteristic->setValue(reinterpret_cast<uint8_t*>(packet.text + offset), count);
        pTxCharacteristic->notify();
    }
}
