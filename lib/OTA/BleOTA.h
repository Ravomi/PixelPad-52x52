#ifndef BLE_OTA_H
#define BLE_OTA_H

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h>
#include <Update.h>
#include <atomic>

class OtaControlCallback;
class OtaDataCallback;
class OtaVersionCallback;
#define ALIYUN_OTA_SERVICE_UUID "0000FEB3-0000-1000-8000-00805F9B34FB"
#define ALIYUN_OTA_RX_UUID      "0000FED7-0000-1000-8000-00805F9B34FB" // WriteNoRsp
#define ALIYUN_OTA_TX_UUID      "0000FED8-0000-1000-8000-00805F9B34FB" // Notify

#define OTA_VERSION_CHAR_UUID    "8000FF01-0000-1000-8000-00805F9B34FB" // Read 4-byte version
#define OTA_PRODUCT_KEY_UUID     "8000FF04-0000-1000-8000-00805F9B34FB" // Read ProductKey

#ifndef ALIYUN_PRODUCT_KEY
#error "ALIYUN_PRODUCT_KEY must be provided via platformio.ini build_flags"
#endif

#ifndef FW_VERSION_MAJOR
#error "FW_VERSION_MAJOR must be provided via platformio.ini build_flags"
#endif

#ifndef FW_VERSION_MINOR
#error "FW_VERSION_MINOR must be provided via platformio.ini build_flags"
#endif

#ifndef FW_VERSION_PATCH
#error "FW_VERSION_PATCH must be provided via platformio.ini build_flags"
#endif

#define OTA_QUEUE_SIZE 32          // buffered packets before backpressure
#define OTA_SESSION_SAVE_INTERVAL 32
#define OTA_IDLE_TIMEOUT_MS 120000 // 2-minute idle timeout before auto-abort

// === OTA 状态机 ===
// BLE 回调只做：解析帧 + 设置轻量标志/入队 + 立即返回
// 所有重操作 (Flash 写入 / CRC / Update.end) 都在 otaTask 中执行
enum class OtaState : uint8_t {
    IDLE,              // 无 OTA 进行中
    UPGRADE_PENDING,   // 已收到升级请求，等待首批数据
    RECEIVING,         // 正在接收 & 写入数据
    FINISH_PENDING,    // 已收到 FINISH，正在排空写队列
    VERIFYING,         // 队列已空，校验 CRC + Update.end()
    COMPLETE,          // 校验通过，等待重启
    ERROR              // 出错，等待清理
};

// Data packet held in the queue
typedef struct {
    uint8_t* payload;               // heap-allocated, freed by consumer
    uint16_t length;
    uint16_t seq;
} ota_data_t;

class BleOTA {
public:
    static void begin(BLEServer* pServer);
    static void sendResponse(const char* msg);
    static void sendFrame(uint8_t cmd, const uint8_t* payload, uint16_t payloadLen);
    static bool isInProgress();
    static void abort();  // 安全中止 OTA（断连/超时时调用）

private:
    friend class OtaControlCallback;
    friend class OtaDataCallback;
    friend class OtaVersionCallback;

    static void otaTask(void* parameter);
    static void processPacket(const std::string& raw);
    static void _stateMachine();    // otaTask 中运行的状态机主循环
    static void _doFinishVerify();  // VERIFYING 状态的处理
    static void _doErrorCleanup();  // ERROR 状态的清理

    static BLECharacteristic* pVersionChar;
    static BLECharacteristic* pProductKeyChar;
    static BLECharacteristic* pRxChar;
    static BLECharacteristic* pTxChar;

    static QueueHandle_t otaQueue;
    static TaskHandle_t otaTaskHandle;
    static Preferences prefs;

    // 状态机
    static std::atomic<OtaState> otaState;

    // 协议状态
    static uint8_t frameHeader;
    static uint16_t expectedSeq;
    static uint16_t totalFrames;
    static uint16_t imageCrc16;
    static size_t totalBytesReceived;
    static size_t totalBytesWritten;
    static size_t expectedFirmwareSize;
    static volatile bool otaWriteError;

    // FINISH 暂存 (BLE 回调设置，otaTask 消费)
    static uint16_t finishExpectedCrc;

    // 超时保护
    static unsigned long lastDataTime;

    static uint32_t versionCode();
    static bool parseFrame(const std::string& raw, uint8_t& cmd, uint16_t& seq,
                           uint16_t& total, const uint8_t*& payload,
                           uint16_t& payloadLen);
    static void handleVersionQuery();
    static void handleUpgradeRequest(const uint8_t* payload, uint16_t payloadLen);
    static void handleDataPacket(uint16_t seq, uint16_t total,
                                 const uint8_t* payload, uint16_t payloadLen);
    static void handleFinish(const uint8_t* payload, uint16_t payloadLen);
    static uint16_t crc16Ccitt(const uint8_t* data, size_t len, uint16_t seed);
    static void saveSession();
    static void clearSession();
};

#endif
