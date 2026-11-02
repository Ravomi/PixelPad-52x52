#ifndef OTA_H
#define OTA_H

#include <Arduino.h>
#include "TransferGate.h"

// 初始化 OTA 服务
void setupOTA();

// OTA 主循环 (必须在 main loop 中调用)
void loopOTA();

#endif