# FM6373C RTOS 3.3.1：蓝牙传图迟缓与连接失去响应

2026-09-14，基于 `c181644`。用户报告鸿蒙 App 连接后传图、改颜色很慢，随后甚至不再更新。

## 复现与原因

Linux BLE 客户端向同一块 FM6373C 板发送 8112 字节 RGB 图片，模拟小包传输。
20 字节分包约 2.95 秒；独立延迟检查测得 3102 ms，超过 2000 ms 的阈值。
每包增加 15 ms 间隔后，一张图约需 6.7 秒，3.3.0 返回 `recv_fail`。
随后连接仍存在，但 `px_patch` 和 `rtos_stats` 不再执行。

原因是接收逻辑把 5 秒作为整张图的总时限，持续进展的慢传也会失败；失步保护随后拒绝
整个连接会话的数据，却没有主动断开，App 因而仍显示已连接。渲染排队延迟约 2 ms，
底层 DMA 扫描约 60 FPS，均不能排除这一传图问题。

当前 App 代码允许鸿蒙使用默认 MTU；本次通过 20 字节分包复现，没有抓取用户手机的实际 MTU 或 HCI 日志。

## 修复

- 连接建立时由外设请求 7.5–15 ms 连接间隔、零从机延迟、4 秒监督超时，最终由中央设备协商决定。
  仅应用这一项时，10 张小包图片的 ACK 中位时间降至 1028 ms。
  参数含义见 [Espressif GAP API](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/bluetooth/esp_gap_ble.html)。
- 图片接收改为 5 秒无进展超时，同时保留 30 秒总时限。待机内容仍保留原有 300 秒总时限。
- 二进制接收失步后排队发送错误通知，约 250 ms 后主动断连。保留会话隔离，晚到的数据不能作为新命令执行。
- `display_stats` 增加 `committed`、`shown`、`shown_hash`。只有目标 DMA 缓冲区确认激活后，才报告
  对应 RGB 快照的 FNV-1a 32 位哈希，以验证新内容确实交给了显示驱动。
- 固件版本更新为 3.3.1。保留 FM6373C 波形、引脚、亮度规则、Wi-Fi AP 和 HTTP/BLE OTA。

## 实板验证

| 场景 | 次数 | 结果 |
| --- | ---: | --- |
| 20 字节分包 | 20 张图及其后的 20 次颜色修改 | 全部通过；ACK 中位 1049.7 ms，最大 1184.3 ms |
| 20 字节分包，每包间隔 15 ms | 2 张图及其后的 2 次颜色修改 | 全部通过；约 6.76 秒，不再误超时 |
| 480 字节分包，显式确认协商尺寸 | 20 张图及其后的 20 次颜色修改 | 全部通过；ACK 中位 194.1 ms，最大 239.9 ms |
| BlueZ 缓存回退到 20 字节后的额外一轮 | 20 张图及其后的 20 次颜色修改 | 全部通过；实际分包尺寸记录在日志中 |
| 部分正文后停止发送 | 1 次 | 无成功 ACK；超时后主动断连，重新连接后指令正常 |
| 碎片 JSON、超长 JSON 恢复、待机参数边界 | 各 1 组 | 全部通过 |

每张图和每次颜色修改均核对 `shown_hash`，而不只检查 ACK 或 DMA FPS。
测试没有新增 RX 溢出、超大包、TX 丢弃或命令/输入队列丢弃。两种固件构建通过，
7 组主机回归测试通过 Clang ASan/UBSan，包括持续进展、空闲超时、总时限及 millis 回绕。

首次最终烧录在全部数据写完后发生串口 I/O 错误；随后使用 115200 波特率执行
`esptool verify_flash`，确认 bootloader、分区表、OTA 引导元数据和应用四个区域全部摘要匹配，再复位启动。
启动日志确认 3.3.1，测试期间 DMA 约 60 FPS。未擦除 NVS 或上传文件系统。

测试命令（`$BOARD_ADDRESS` 指显式选择的测试板）：

```sh
CXX=clang++ python tools/test_host.py --sanitize
pio run -e pixelpad_s3_fm6373c_rtos
pio run -e pixelpad_s3_fm6373c_ble_only
python tools/ble_image_latency.py --address "$BOARD_ADDRESS" --chunk 20 --frames 20
python tools/ble_image_latency.py --address "$BOARD_ADDRESS" --chunk 20 --gap-ms 15 --frames 2 --max-ms 15000
python tools/ble_image_latency.py --address "$BOARD_ADDRESS" --chunk 480 --frames 20 --negotiate-mtu --require-chunk
python tools/ble_smoke.py --address "$BOARD_ADDRESS"
```

[验证清单与固件 SHA256](rtos-validation/3.3.1/summary.json) 同目录保存完整 BLE 和主机测试记录。
ACK 时间包含主机发送、蓝牙传输和渲染接收，哈希只证明交给活动 DMA 缓冲区的源图正确，
不能替代屏幕光学测量。尚未用用户鸿蒙手机复测，也未在此补丁中做实体按键或完整 OTA 回归。
