#include "BleOTA.h"
#include "TransferGate.h"
#include <WiFi.h>
#include "Bluetooth.h"
extern Bluetooth ble;

BLECharacteristic* BleOTA::pVersionChar = nullptr;
BLECharacteristic* BleOTA::pProductKeyChar = nullptr;
BLECharacteristic* BleOTA::pRxChar = nullptr;
BLECharacteristic* BleOTA::pTxChar = nullptr;
QueueHandle_t BleOTA::otaQueue = nullptr;
TaskHandle_t BleOTA::otaTaskHandle = nullptr;
Preferences BleOTA::prefs;

std::atomic<OtaState> BleOTA::otaState{OtaState::IDLE};
uint8_t BleOTA::frameHeader = 0xA5;
uint16_t BleOTA::expectedSeq = 1;
uint16_t BleOTA::totalFrames = 0;
uint16_t BleOTA::imageCrc16 = 0xFFFF;
size_t BleOTA::totalBytesReceived = 0;
size_t BleOTA::totalBytesWritten = 0;
size_t BleOTA::expectedFirmwareSize = 0;
volatile bool BleOTA::otaWriteError = false;
uint16_t BleOTA::finishExpectedCrc = 0;
unsigned long BleOTA::lastDataTime = 0;

static const uint8_t CMD_QUERY_VERSION = 0x20;
static const uint8_t CMD_VERSION_RSP = 0x21;
static const uint8_t CMD_UPGRADE_REQ = 0x22;
static const uint8_t CMD_UPGRADE_RSP = 0x23;
static const uint8_t CMD_DATA_RSP = 0x24;
static const uint8_t CMD_FINISH = 0x25;
static const uint8_t CMD_FINISH_RSP = 0x26;
static const uint8_t CMD_DATA_PACKET = 0x2F;

static const uint8_t OTA_OK = 0x00;
static const uint8_t OTA_ERR_PARAM = 0x01;
static const uint8_t OTA_ERR_SPACE = 0x02;
static const uint8_t OTA_ERR_SEQ = 0x03;
static const uint8_t OTA_ERR_WRITE = 0x04;
static const uint8_t OTA_ERR_CRC = 0x05;

// ---------------------------------------------------------------------------
// BLE callbacks — 只做轻量操作，立即返回，不阻塞
// ---------------------------------------------------------------------------

class OtaVersionCallback : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* pChar) override {
    const uint32_t v = BleOTA::versionCode();
    uint8_t raw[4] = {
      static_cast<uint8_t>((v >> 24) & 0xFF),
      static_cast<uint8_t>((v >> 16) & 0xFF),
      static_cast<uint8_t>((v >> 8) & 0xFF),
      static_cast<uint8_t>(v & 0xFF)
    };
    pChar->setValue(raw, sizeof(raw));
  }
};

class ProductKeyCallback : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* pChar) override {
    pChar->setValue(ALIYUN_PRODUCT_KEY);
  }
};

namespace {
struct OtaRawPacket {
  uint32_t session;
  uint16_t size;
  uint8_t data[520];
};
QueueHandle_t rawQueue = nullptr;
std::atomic<bool> abortRequested{false};
std::atomic<uint32_t> rejectedSession{0};
uint32_t workerSession = 0;
}

class OtaDataCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    const std::string value = characteristic->getValue();
    if (value.empty()) return;
    OtaRawPacket packet{};
    packet.session = ble.session();
    if (rejectedSession == packet.session) return;
    if (value.size() > sizeof(packet.data)) { rejectedSession = packet.session; abortRequested = true; return; }
    packet.size = value.size();
    memcpy(packet.data, value.data(), value.size());
    if (xQueueSend(rawQueue, &packet, 0) != pdTRUE) {
      rejectedSession = packet.session;
      abortRequested = true;
    }
  }
};

// Only the OTA worker parses commands, mutates Update/Preferences, and sends
// OTA notifications. BLE callbacks copy a bounded packet and return immediately.
void BleOTA::processPacket(const std::string& rxValue) {
    uint8_t cmd = 0;
    uint16_t seq = 0;
    uint16_t total = 0;
    uint16_t payloadLen = 0;
    const uint8_t* payload = nullptr;
    if (!BleOTA::parseFrame(rxValue, cmd, seq, total, payload, payloadLen)) {
      uint8_t rsp[3] = {OTA_ERR_PARAM, 0x00, 0x00};
      BleOTA::sendFrame(CMD_DATA_RSP, rsp, sizeof(rsp));
      return;
    }

    // version query 和 upgrade request 在任何状态都可以处理
    // data packet 和 finish 需要状态机检查
    switch (cmd) {
      case CMD_QUERY_VERSION:
        BleOTA::handleVersionQuery();
        break;
      case CMD_UPGRADE_REQ:
        BleOTA::handleUpgradeRequest(payload, payloadLen);
        break;
      case CMD_DATA_PACKET:
        BleOTA::handleDataPacket(seq, total, payload, payloadLen);
        break;
      case CMD_FINISH:
        BleOTA::handleFinish(payload, payloadLen);
        break;
      default: {
        uint8_t rsp[3] = {OTA_ERR_PARAM, 0x00, 0x00};
        BleOTA::sendFrame(CMD_DATA_RSP, rsp, sizeof(rsp));
      } break;
    }
}

// ---------------------------------------------------------------------------
// otaTask — 状态机主循环，所有重操作都在这里执行
// ---------------------------------------------------------------------------

void BleOTA::otaTask(void* parameter) {
  (void)parameter;
  while (true) {
    if (abortRequested.exchange(false)) {
      if (ble.connected() && rejectedSession == ble.session()) {
        workerSession = ble.session();
        const uint8_t response[] = {OTA_ERR_WRITE, 0, 0};
        sendFrame(CMD_DATA_RSP, response, sizeof(response));
      }
      _doErrorCleanup();
    }
    OtaRawPacket packet;
    // A queue arrival wakes the idle worker immediately (formerly 500 ms sleep).
    if (xQueueReceive(rawQueue, &packet, pdMS_TO_TICKS(10)) == pdTRUE) {
      if (ble.connected() && packet.session == ble.session() && rejectedSession != packet.session) {
        if (workerSession != packet.session) {
          _doErrorCleanup();
          workerSession = packet.session;
        }
        processPacket(std::string(reinterpret_cast<char*>(packet.data), packet.size));
      }
    }
    if (!ble.connected() || workerSession != ble.session()) _doErrorCleanup();
    _stateMachine();
  }
}

void BleOTA::_stateMachine() {
  OtaState state = otaState; // 快照当前状态

  switch (state) {
    case OtaState::IDLE:
      // 空闲，降低 CPU 占用
      return;

    case OtaState::UPGRADE_PENDING:
    case OtaState::RECEIVING: {
      // The outer raw queue wait supplies the idle timeout; this queue never blocks.
      ota_data_t item;
      if (xQueueReceive(otaQueue, &item, 0) != pdTRUE) {
        // 超时：检查是否 idle
        if (millis() - lastDataTime > OTA_IDLE_TIMEOUT_MS) {
          Serial.println("BLE OTA: idle timeout, aborting");
          _doErrorCleanup();
        }
        return;
      }

      // 进入 RECEIVING 状态
      if (state == OtaState::UPGRADE_PENDING) {
        otaState = OtaState::RECEIVING;
      }

      const size_t written = Update.write(item.payload, item.length);
      if (written != item.length) {
        Serial.printf("BLE OTA write err seq=%u: %u/%u\n",
                      item.seq, (unsigned)written, item.length);
        otaWriteError = true;
        free(item.payload);
        return;
      }

      totalBytesWritten += written;
      imageCrc16 = crc16Ccitt(item.payload, written, imageCrc16);

      // 进度日志
      if (expectedFirmwareSize > 0 && (totalBytesWritten % (1024 * 50) < 512)) {
        Serial.printf("BLE OTA: %u/%u (%.1f%%)\n",
                      (unsigned)totalBytesWritten,
                      (unsigned)expectedFirmwareSize,
                      (float)totalBytesWritten * 100.0f / (float)expectedFirmwareSize);
      }

      // 周期性 session 保存
      const bool isLast = (item.seq == totalFrames);
      const bool burst = (item.seq % OTA_SESSION_SAVE_INTERVAL == 0);
      if (isLast || burst) saveSession();

      // ★ 修复: ACK 发送 expectedSeq（下一个期望帧号），与错误路径语义一致
      // expectedSeq 在 handleDataPacket 中已经递增，所以它表示"下一个期望帧"
      if (isLast || burst) {
        uint8_t rsp[3] = {OTA_OK,
                          (uint8_t)(expectedSeq & 0xFF),
                          (uint8_t)((expectedSeq >> 8) & 0xFF)};
        sendFrame(CMD_DATA_RSP, rsp, sizeof(rsp));
      }

      free(item.payload);
      return;
    }

    case OtaState::FINISH_PENDING: {
      // 继续处理队列中残留的数据包（写 Flash，不丢弃！）
      ota_data_t item;
      if (xQueueReceive(otaQueue, &item, 0) == pdTRUE) {
        const size_t written = Update.write(item.payload, item.length);
        if (written != item.length) {
          Serial.printf("BLE OTA finish-drain write err seq=%u\n", item.seq);
          otaWriteError = true;
        }
        totalBytesWritten += written;
        imageCrc16 = crc16Ccitt(item.payload, written, imageCrc16);
        free(item.payload);
        return; // 还有数据要处理
      }

      // 队列已空，进入校验阶段
      otaState = OtaState::VERIFYING;
      Serial.println("BLE OTA: queue drained, verifying...");
      return;
    }

    case OtaState::VERIFYING:
      _doFinishVerify();
      return;

    case OtaState::COMPLETE:
      // 一次性操作：发送响应 + 重启
      Serial.println("BLE OTA success — restarting");
      {
        uint8_t rsp[1] = {OTA_OK};
        sendFrame(CMD_FINISH_RSP, rsp, sizeof(rsp));
      }
      clearSession();
      delay(300);
      ESP.restart();
      return;

    case OtaState::ERROR:
      _doErrorCleanup();
      return;
  }
}

void BleOTA::_doFinishVerify() {
  if (otaWriteError) {
    Serial.println("BLE OTA verify: write error during transfer");
    Update.abort();
    {
      uint8_t rsp[1] = {OTA_ERR_WRITE};
      sendFrame(CMD_FINISH_RSP, rsp, sizeof(rsp));
    }
    _doErrorCleanup();
    return;
  }

  if (totalBytesWritten != expectedFirmwareSize || imageCrc16 != finishExpectedCrc) {
    Serial.printf("BLE OTA CRC mismatch: 0x%04X vs 0x%04X\n",
                  imageCrc16, finishExpectedCrc);
    Update.abort();
    {
      uint8_t rsp[1] = {OTA_ERR_CRC};
      sendFrame(CMD_FINISH_RSP, rsp, sizeof(rsp));
    }
    _doErrorCleanup();
    return;
  }

  if (!Update.end(true)) {
    Serial.printf("BLE OTA Update.end failed: %s (err=%u)\n",
                  Update.errorString(), Update.getError());
    {
      uint8_t rsp[1] = {OTA_ERR_WRITE};
      sendFrame(CMD_FINISH_RSP, rsp, sizeof(rsp));
    }
    _doErrorCleanup();
    return;
  }

  // 全部通过！
  otaState = OtaState::COMPLETE;
}

static wifi_mode_t previousWifiMode=WIFI_OFF;
static bool wifiSuspended=false;

void BleOTA::_doErrorCleanup() {
  if (otaState != OtaState::IDLE) {
    Update.abort();
    clearSession();

    // 清空队列中残留的数据包
    ota_data_t item;
    while (xQueueReceive(otaQueue, &item, 0) == pdTRUE) {
      free(item.payload);
    }

    Serial.println("BLE OTA: error cleanup complete");
  }
  if (wifiSuspended) { WiFi.mode(previousWifiMode); wifiSuspended=false; }
  transferGate.release(TransferOwner::BleOta);
  otaState = OtaState::IDLE;
  expectedSeq = 1;
  totalFrames = 0;
  imageCrc16 = 0xFFFF;
  totalBytesReceived = 0;
  totalBytesWritten = 0;
  expectedFirmwareSize = 0;
  otaWriteError = false;
  finishExpectedCrc = 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool BleOTA::isInProgress() {
  OtaState s = otaState;
  return s != OtaState::IDLE && s != OtaState::ERROR;
}

void BleOTA::abort() {
  abortRequested = true;
}

uint32_t BleOTA::versionCode() {
  return ((uint32_t)FW_VERSION_MAJOR << 16)
       | ((uint32_t)FW_VERSION_MINOR << 8)
       |  (uint32_t)FW_VERSION_PATCH;
}

bool BleOTA::parseFrame(const std::string& raw, uint8_t& cmd, uint16_t& seq,
                         uint16_t& total, const uint8_t*& payload,
                         uint16_t& payloadLen) {
  if (raw.size() < 8) return false;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(raw.data());
  if (p[0] != frameHeader) return false;
  cmd = p[1];
  total = (uint16_t)(p[2] | ((uint16_t)p[3] << 8));
  seq   = (uint16_t)(p[4] | ((uint16_t)p[5] << 8));
  payloadLen = (uint16_t)(p[6] | ((uint16_t)p[7] << 8));
  if (8u + payloadLen > raw.size()) return false;
  payload = p + 8;
  return true;
}

void BleOTA::sendFrame(uint8_t cmd, const uint8_t* payload, uint16_t payloadLen) {
  if (!pTxChar || !ble.connected() || workerSession != ble.session()) return;
  const size_t frameLen = 8u + payloadLen;
  uint8_t* frame = (uint8_t*)malloc(frameLen);
  if (!frame) return;
  frame[0] = frameHeader;
  frame[1] = cmd;
  frame[2] = 1; frame[3] = 0;
  frame[4] = 1; frame[5] = 0;
  frame[6] = (uint8_t)(payloadLen & 0xFF);
  frame[7] = (uint8_t)((payloadLen >> 8) & 0xFF);
  if (payloadLen > 0 && payload) memcpy(frame + 8, payload, payloadLen);
  pTxChar->setValue(frame, frameLen);
  pTxChar->notify();
  free(frame);
}

void BleOTA::sendResponse(const char* msg) {
  sendFrame(CMD_DATA_RSP, (const uint8_t*)msg, (uint16_t)strlen(msg));
}

// ---------------------------------------------------------------------------
// OTA worker command processing; Update, NVS and state have one owner
// ---------------------------------------------------------------------------

void BleOTA::handleVersionQuery() {
  uint8_t pld[5];
  pld[0] = 0x00;
  uint32_t v = versionCode();
  pld[1] = (uint8_t)((v >> 24) & 0xFF);
  pld[2] = (uint8_t)((v >> 16) & 0xFF);
  pld[3] = (uint8_t)((v >> 8)  & 0xFF);
  pld[4] = (uint8_t)( v        & 0xFF);
  sendFrame(CMD_VERSION_RSP, pld, sizeof(pld));
}

void BleOTA::handleUpgradeRequest(const uint8_t* payload, uint16_t payloadLen) {
  // 只在空闲或已清理错误时接受升级请求
  OtaState s = otaState;
  if (s != OtaState::IDLE && s != OtaState::ERROR) {
    uint8_t rsp[3] = {OTA_ERR_PARAM, 0, 0};
    sendFrame(CMD_UPGRADE_RSP, rsp, sizeof(rsp));
    return;
  }

  if (payloadLen < 9) {
    uint8_t rsp[3] = {OTA_ERR_PARAM, 0, 0};
    sendFrame(CMD_UPGRADE_RSP, rsp, sizeof(rsp));
    return;
  }

  uint32_t targetVer = ((uint32_t)payload[1] << 24)
                     | ((uint32_t)payload[2] << 16)
                     | ((uint32_t)payload[3] << 8)
                     |  (uint32_t)payload[4];
  uint32_t fwSize    =  (uint32_t)payload[5]
                     | ((uint32_t)payload[6] << 8)
                     | ((uint32_t)payload[7] << 16)
                     | ((uint32_t)payload[8] << 24);

  if (targetVer <= versionCode() || fwSize == 0) {
    uint8_t rsp[3] = {OTA_ERR_PARAM, 0, 0};
    sendFrame(CMD_UPGRADE_RSP, rsp, sizeof(rsp));
    return;
  }

  if (!transferGate.claim(TransferOwner::BleOta)) {
    uint8_t rsp[3] = {OTA_ERR_WRITE, 0, 0};
    sendFrame(CMD_UPGRADE_RSP, rsp, sizeof(rsp));
    return;
  }
  otaState = OtaState::UPGRADE_PENDING; // Publish busy before any slow work.
  previousWifiMode=WiFi.getMode(); wifiSuspended=true;

  // 关闭 WiFi 以释放资源
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);

  Update.abort();
  if (!Update.begin(fwSize, U_FLASH)) {
    Serial.printf("BLE OTA Update.begin (1st): %s (err=%u)\n", Update.errorString(), Update.getError());
    Update.abort();
    delay(100);
    if (!Update.begin(fwSize, U_FLASH)) {
      Serial.printf("BLE OTA Update.begin (2nd): %s (err=%u)\n", Update.errorString(), Update.getError());
      uint8_t rsp[3] = {OTA_ERR_SPACE, 0, 0};
      sendFrame(CMD_UPGRADE_RSP, rsp, sizeof(rsp));
      _doErrorCleanup();
      return;
    }
  }

  // 初始化所有状态变量
  expectedFirmwareSize = fwSize;
  totalBytesReceived   = 0;
  totalBytesWritten    = 0;
  expectedSeq          = 1;
  totalFrames          = 0;
  imageCrc16           = 0xFFFF;
  otaWriteError        = false;
  finishExpectedCrc    = 0;
  lastDataTime         = millis();
  clearSession();

  // ★ 状态转移：进入 UPGRADE_PENDING
  otaState = OtaState::UPGRADE_PENDING;

  uint8_t rsp[5];
  rsp[0] = OTA_OK;
  rsp[1] = 0; rsp[2] = 0; rsp[3] = 0; rsp[4] = 0;
  sendFrame(CMD_UPGRADE_RSP, rsp, sizeof(rsp));

  Serial.printf("BLE OTA: upgrade accepted, fwSize=%u\n", (unsigned)fwSize);
}

// OTA worker validates a data packet before writing it
void BleOTA::handleDataPacket(uint16_t seq, uint16_t total,
                               const uint8_t* payload, uint16_t payloadLen) {
  OtaState s = otaState;
  if (s != OtaState::UPGRADE_PENDING && s != OtaState::RECEIVING) {
    uint8_t rsp[3] = {OTA_ERR_PARAM,
                      (uint8_t)(expectedSeq & 0xFF),
                      (uint8_t)((expectedSeq >> 8) & 0xFF)};
    sendFrame(CMD_DATA_RSP, rsp, sizeof(rsp));
    return;
  }

  if (!payload || payloadLen == 0 || payloadLen > 512 ||
      total == 0 || seq == 0 || seq > total ||
      totalBytesReceived + payloadLen > expectedFirmwareSize) {
    uint8_t rsp[3] = {OTA_ERR_PARAM,
                      (uint8_t)(expectedSeq & 0xFF),
                      (uint8_t)((expectedSeq >> 8) & 0xFF)};
    sendFrame(CMD_DATA_RSP, rsp, sizeof(rsp));
    return;
  }

  if (seq != expectedSeq) {
    uint8_t rsp[3] = {OTA_ERR_SEQ,
                      (uint8_t)(expectedSeq & 0xFF),
                      (uint8_t)((expectedSeq >> 8) & 0xFF)};
    sendFrame(CMD_DATA_RSP, rsp, sizeof(rsp));
    return;
  }

  // 分配内存 + 入队
  ota_data_t item;
  item.payload = (uint8_t*)malloc(payloadLen);
  if (!item.payload) {
    uint8_t rsp[3] = {OTA_ERR_WRITE,
                      (uint8_t)(expectedSeq & 0xFF),
                      (uint8_t)((expectedSeq >> 8) & 0xFF)};
    sendFrame(CMD_DATA_RSP, rsp, sizeof(rsp));
    return;
  }
  memcpy(item.payload, payload, payloadLen);
  item.length = payloadLen;
  item.seq    = seq;

  // The same worker drains this queue after processing each raw packet.
  if (xQueueSend(otaQueue, &item, 0) != pdTRUE) {
    free(item.payload);
    uint8_t rsp[3] = {OTA_ERR_WRITE,
                      (uint8_t)(expectedSeq & 0xFF),
                      (uint8_t)((expectedSeq >> 8) & 0xFF)};
    sendFrame(CMD_DATA_RSP, rsp, sizeof(rsp));
    return;
  }

  totalBytesReceived += payloadLen;
  expectedSeq++;
  totalFrames = total;
  lastDataTime = millis(); // 更新最后活跃时间
}

// OTA worker records FINISH and completes verification through the state machine
void BleOTA::handleFinish(const uint8_t* payload, uint16_t payloadLen) {
  OtaState s = otaState;
  if (s != OtaState::RECEIVING) {
    Serial.printf("BLE OTA finish rejected: state=%d\n", (int)s);
    {
      uint8_t rsp[1] = {OTA_ERR_PARAM};
      sendFrame(CMD_FINISH_RSP, rsp, sizeof(rsp));
    }
    if (s == OtaState::UPGRADE_PENDING) {
      // 没有任何数据就收到 FINISH
      _doErrorCleanup();
    }
    return;
  }

  if (payload == nullptr || payloadLen < 2) {
    Serial.println("BLE OTA finish rejected: missing CRC");
    {
      uint8_t rsp[1] = {OTA_ERR_CRC};
      sendFrame(CMD_FINISH_RSP, rsp, sizeof(rsp));
    }
    otaState = OtaState::ERROR;
    return;
  }

  // ★ 关键: 只记录 CRC + 切换状态，不阻塞，不等待队列！
  finishExpectedCrc = (uint16_t)(payload[0] | ((uint16_t)payload[1] << 8));
  otaState = OtaState::FINISH_PENDING;

  Serial.printf("BLE OTA: finish requested, CRC=0x%04X, "
                "queue_depth=%d, transitioning to FINISH_PENDING\n",
                finishExpectedCrc,
                (int)uxQueueMessagesWaiting(otaQueue));
  // handleFinish 立即返回！otaTask 的状态机会处理后续
}

// ---------------------------------------------------------------------------
// CRC-16-CCITT
// ---------------------------------------------------------------------------

uint16_t BleOTA::crc16Ccitt(const uint8_t* data, size_t len, uint16_t seed) {
  uint16_t crc = seed;
  for (size_t i = 0; i < len; ++i) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t b = 0; b < 8; ++b) {
      if (crc & 0x8000) crc = (uint16_t)((crc << 1) ^ 0x1021);
      else              crc <<= 1;
    }
  }
  return crc;
}

// ---------------------------------------------------------------------------
// NVS session persistence
// ---------------------------------------------------------------------------

void BleOTA::saveSession() {
  prefs.putULong("bytes", (uint32_t)totalBytesWritten);
  prefs.putUShort("seq", expectedSeq);
  prefs.putULong("size", (uint32_t)expectedFirmwareSize);
}

void BleOTA::clearSession() {
  prefs.putULong("bytes", 0);
  prefs.putUShort("seq", 1);
  prefs.putULong("size", 0);
}

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------

void BleOTA::begin(BLEServer* pServer) {
  if (!pServer) return;

  prefs.begin("bleota", false);

  otaQueue = xQueueCreate(OTA_QUEUE_SIZE, sizeof(ota_data_t));
  rawQueue = xQueueCreate(OTA_QUEUE_SIZE, sizeof(OtaRawPacket));
  if (!otaQueue || !rawQueue) {
    Serial.println("BLE OTA: queue create failed");
    ::abort();
  }
  // 增加栈大小到 10KB，状态机需要更多栈空间
  if (xTaskCreatePinnedToCore(otaTask, "pp_ota", 10240, nullptr, 3, &otaTaskHandle, 0) != pdPASS) {
    Serial.println("BLE OTA: task create failed");
    ::abort();
  }

  BLEService* svc = pServer->createService(ALIYUN_OTA_SERVICE_UUID);

  pVersionChar = svc->createCharacteristic(OTA_VERSION_CHAR_UUID, BLECharacteristic::PROPERTY_READ);
  pVersionChar->setCallbacks(new OtaVersionCallback());

  pProductKeyChar = svc->createCharacteristic(OTA_PRODUCT_KEY_UUID, BLECharacteristic::PROPERTY_READ);
  pProductKeyChar->setCallbacks(new ProductKeyCallback());
  pProductKeyChar->setValue(ALIYUN_PRODUCT_KEY);

  pRxChar = svc->createCharacteristic(ALIYUN_OTA_RX_UUID, BLECharacteristic::PROPERTY_WRITE_NR);
  pRxChar->setCallbacks(new OtaDataCallback());

  pTxChar = svc->createCharacteristic(ALIYUN_OTA_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  pTxChar->addDescriptor(new BLE2902());

  svc->start();

  // 检查是否有上次未完成的 OTA 会话
  expectedFirmwareSize = prefs.getULong("size", 0);
  totalBytesReceived   = prefs.getULong("bytes", 0);
  expectedSeq          = prefs.getUShort("seq", 1);

  if (totalBytesReceived > 0) {
    Serial.printf("BLE OTA: stale session (%u/%u bytes) — cleared\n",
                  (unsigned)totalBytesReceived, (unsigned)expectedFirmwareSize);
    clearSession();
  }

  otaState = OtaState::IDLE;

  Serial.println("Aliyun BLE OTA service mounted (state-machine, async write)");
  Serial.printf("  Version: %u.%u.%u (0x%08X)\n",
                FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH, versionCode());
  Serial.printf("  Product key: %s\n", ALIYUN_PRODUCT_KEY);
}
