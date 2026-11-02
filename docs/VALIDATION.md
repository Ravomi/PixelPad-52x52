# RTOS 3.2.3 验证记录

> 本文记录之前的 3.2.3。后续 3.2.4 已另外烧录和测试，参见 [高刷验证记录](HIGH_REFRESH_VALIDATION.md)；下列历史结果不代表新版测试结果。

日期：2026-09-14。工程已建立并实际烧录到 ESP32-S3；采用 ESP-IDF FreeRTOS + Arduino 2.0.17 驱动兼容层。

## 编译与主机测试

- 默认 `pixelpad_s3_rtos` 与可选 `pixelpad_s3_ble_only` 均通过 PlatformIO 构建。实际烧录的是默认环境，保留 Wi-Fi AP / HTTP OTA。
- 默认固件构建报告：静态 RAM 83,348 / 327,680 字节；Flash 2,438,845 / 3,145,728 字节。运行时堆/任务栈不等于构建时静态 RAM，另见实板统计。
- Clang C++11、`-Wall -Wextra -Werror`、AddressSanitizer + UndefinedBehaviorSanitizer：协议边界、亮度策略和生命游戏重绘回归全部通过。运行命令：`CXX=clang++ python tools/test_host.py --sanitize`。
- Python 测试/串口工具通过语法检查。构建中的 FastLED 可选并行驱动提示来自现有依赖，HUB75 使用独立 DMA 库。

## 烧录与备份

设备：ESP32-S3 QFN56 v0.2，16 MB Flash，8 MB 内置 PSRAM；芯片 MAC `AA:BB:CC:DD:EE:00`；蓝牙地址 `AA:BB:CC:DD:EE:01`；广播名 `PixelPad-XXXX`；串口 `/dev/ttyUSB0`（CH340）。

备份文件：`device-backups/52-before-rtos/boot-nvs-app0-0x310000.bin`。

- 范围 `[0x0, 0x310000)`，共 3,211,264 字节，覆盖本次写入的启动区、分区表、OTA 选择数据和 app0 槽，也包含原 NVS。
- SHA-256：`a598628217a2b63027efb468991315a3428f759b37606ae94f41af509cfee9f7`。
- app1 和文件系统未备份，本次串口烧录也未改写这两个区域；没有执行整片擦除或 uploadfs。
- 921600 读取出现数据错误，460800 读取成功。第一次 460800 写入中断，随后重试成功；最终版本再次完整写入并通过每个写入段的 Flash 哈希校验。见 [最终烧录日志](validation/upload-final.log)。

启动后可用堆约 66 KB，PSRAM 可用约 8.3 MB；所有显式任务创建成功，BLE 广播与命令正常。发现并修正原代码 `analogRead(0)` 在 ESP32-S3 上无效的问题，改用 `esp_random()`。

启动仍可见未配置 core dump 分区、未保存 BLE 名称命名空间、无自定义待机文件等提示。这些提示不代表 RTOS 启动失败，本次没有为了清除提示去重分区或格式化设备。

## 实板功能与指标

以下测试在默认固件、临时 **10% 亮度**下完成，使用本机 BlueZ/Bleak。图片测试会替换当前显示内容。

| 检查 | 结果 | 记录 |
| --- | --- | --- |
| 连续 100 张 8112 字节 RGB 图片 | 100/100 收到成功 ACK，累计 811,200 字节；同一连接中完成 | [100 张记录](validation/ble-image-100.json) |
| JSON 跨写入分片 | 通过 | [协议检查](validation/ble-smoke.log) |
| 待机帧数超范围 | 返回 invalid_params，没有进入正文接收 | 同上 |
| 超长行包含合法 JSON 尾部 | 拒绝整行，后续正常命令仍可执行 | 同上 |
| 正文含 NUL/LF/CR，随后停止发送 | 等到接收超时，返回 recv_fail，没有成功 ACK | 同上 |
| 二进制超时后重新连接 | 重新接收正常命令，无旧会话污染 | 同上 |
| 补充 10 张图片与 5 次统计查询 | 均完成 | [补充传图](validation/ble-image-negotiated.jsonl)、[最终统计](validation/ble-stats-final.jsonl) |

最终统计快照：

- `commands = 119`，计数与传图/测试命令连续一致。
- 命令入队到处理最大等待 **1951 µs**；显示任务单轮最大处理时间 **5678 µs**。
- `rx_overflows = tx_drops = input_drops = command_drops = 0`。
- 可用堆约 **62,980 字节**，历史最低 **53,432 字节**。
- input/render/protocol/service 的历史最小剩余栈分别 **2380 / 8692 / 5020 / 3428 字节**。
- 最终测试没有人为触发输入，`input_events=0`，因此不能用该轮的 `input_max_us=0` 宣称零延迟。较早一次启动统计记录过 3 个输入事件、最大队列等待 4995 µs，见 [初步统计](validation/ble-stats-initial.jsonl)；不把它当作完整按键手感验收。

100 张传图在当前测试主机上的中位数 **2694.310 ms**，P95 **2964.550 ms**。补充 10 张中位数 **2065.107 ms**。后续脚本记录显示 Bleak 公共接口提供的可写分包长度仍为 **20 字节**，虽然 BlueZ GATT 属性显示连接 MTU 为 517；测试没有绕过客户端报告的可写上限。该数据包含主机分包/间隔、无线传输和 ACK，不是 MCU 队列处理耗时，也不能用两轮差异宣称 RTOS 提速百分比。正常 App 协商大分包后的吞吐需另测。

统计查询的 BLE 往返中位数约 85 ms，亦明显大于 MCU 内部约 2 ms 的队列等待；应区分调度响应和无线端到端响应。

## 欠压复查与边界

- 在烧录新固件**之前**，原设备已反复记录 `BROWNOUT_RST`，见 [旧固件串口记录](validation/before-rtos-uart.log)。
- 用户确认外接电源后完成烧录；最终固件最初启动监测中仍有一次欠压复位，见 [启动记录](validation/rtos-final-boot-uart.log)。
- 随后的低亮度复查，50 秒串口记录无新的欠压、Guru Meditation、回溯或 watchdog 报错，见 [传图期间串口记录](validation/rtos-ble-test-uart.log)。连续 100 张传图连接保持，统计计数连续。
- 这说明该轮低亮度测试运行稳定，**不代表已经定位或根治供电问题**。没有关闭 brownout 保护，也没有用软件吞掉复位信息。设备测试结束保留 10% 亮度与低亮度测试画面。

尚未完成：满亮度/长按全白功耗验收、全部游戏长期压力测试、目视像素映射/显示质量确认、待机文件持久化故障测试、BLE/HTTP OTA 完整升级与回滚、跨升级通道仲裁，以及同一电源/同一客户端下旧固件与新固件的端到端对照测试。

以上范围足以确认这版固件能编译、烧录、运行任务和完成基础 BLE 收发；不替代量产或长期稳定性验收。
