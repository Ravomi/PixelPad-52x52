# FM6373C RTOS 3.3.0

> 本文记录 3.3.0 迁移基线。当前 3.3.1 的蓝牙小包传输、超时及主动断连变更和实板结果见 [修复记录](BLE_TRANSFER_FIX_3.3.1.md)。

日期：2026-09-14。目标分支：`firmware/52x52-fm6373c`。
迁移基线：本分支 `c5dab155`，RTOS 参考 `firmware/52x52-rtos` 的 `2d6c434`。

## 实现

保留 ESP32-S3 / Arduino 2.0.17 驱动兼容层，将业务主循环改为显式 FreeRTOS 任务。
`loop()` 休眠；原 FM6373C 波形协议、引脚、PSRAM DMA、8 MHz DCLK、26 扫及 UART 诊断保留。
没有套用 FM6124 的 16 MHz/8 bit 位平面参数；FM6373C 仍使用自身内部 PWM 灰度和扫描协议。

| 任务 | 核 | 优先级 | 栈申请（字节） | 工作 |
| --- | --- | --- | --- | --- |
| pp_input | 1 | 4 | 3072 | 每 2 ms 采样按键/旋钮，仅投递事件 |
| pp_render | 1 | 2 | 12288 | 独占 App、游戏和应用画面的更新 |
| pp_protocol | 0 | 3 | 8192 | JSON、图片正文、待机文件及名称保存 |
| pp_service | 0 | 1 | 6144 | NUS 通知、广播恢复、HTTP OTA 维护 |
| pp_ota | 0 | 3 | 10240 | BLE OTA 命令、Flash、CRC 与升级状态 |
| fm6373c | 未固定 | 1 | 4096 | 取完整画面快照，编码波形、切换 DMA 和 UART 诊断 |

`PublishedFrame` 分开保存绘制中与已提交的 RGB 数据。应用/游戏内部的
`refreshDisplay()` 不发布半帧；渲染任务每轮结束调用 `present()`，在同一互斥锁下提交完整画面。
扫描任务复制已提交画面到独立快照，编码期间渲染任务可以继续绘制。
RGB 数据因此比旧版增加一帧 8112 字节；DMA 描述符和波形分配策略保持原样。

输入队列 32 项，命令队列 8 项；BLE RX 为 64 个带会话编号的包，TX 为 16 个通知包。
采样任务不进行 Flash 写入、Notify、绘图或动态分配。队列和任务申请失败终止启动，
显示初始化成功且 RTOS 任务创建完成后才确认 OTA 启动成功。

## 协议兼容

UUID、型号 1、52×52 RGB888、亮度上限、游戏/模式、旋钮事件、名称和 BLE OTA 保留。

- `recv` 的 `size` 必须为 8112。上一帧保留到正文完整；成功响应带 `cmd:recv`，保留 `status:ok`。
- `set_standby` 为 1–256 帧、1–60 FPS，`size=frames*8112`。先分配内存再发 READY，存储和渲染交接成功后发 OK。
- 超长 JSON 整行丢弃；接收超时、溢出或二进制失步后必须重连。旧连接数据、排队命令与应答不能转移到新连接。
- `game`/`key` 输入形式及 `cmd:key` 均可处理。
- 新增 `{"cmd":"rtos_stats"}` 和 `{"cmd":"display_stats"}`，均需追加换行。
- `rtos_stats` 的 `input_max_us`、`command_max_us` 是排队最大时间，`render_max_us` 是渲染迭代最大时间；
  `stack_free` 顺序为 input/render/protocol/service。统计不等于 GPIO 到屏幕发光的延迟。
- `display_stats` 返回 FM6373C 驱动名、尺寸、时钟、DMA 帧计数和可用状态。

新增共享传输所有权：媒体接收/保存、BLE OTA 和 HTTP OTA 互斥。
HTTP 保留 `/update` UI、固件 `/ota/start` / `/ota/upload` 及 `/raw_upload`；
原始上传首包也计入正文，异常/断线/30 秒无进展时释放会话；成功提交后延后重启。
HTTP 文件系统上传被拒绝，避免覆盖保存的待机内容。BLE OTA 失败后恢复此前 Wi-Fi 模式。

LittleFS 挂载失败不会自动格式化。格式、权限及上层 API 见根目录 `BLE_PROTOCOL.md`；
上述 RTOS 约束优先于早期实现描述。

## 构建与验证

```sh
pio run -e pixelpad_s3_fm6373c_rtos
pio run -e pixelpad_s3_fm6373c_ble_only
CXX=clang++ python tools/test_host.py --sanitize
```

默认保留 Wi-Fi AP / HTTP OTA；BLE 专用构建关闭这两项，保留 BLE OTA。
旧目标 `4d_systems_esp32s3_gen4_r8n16` 是默认 RTOS 环境的兼容别名。

本次结果见 [验证清单](rtos-validation/summary.json) 和 [主机测试日志](rtos-validation/host-tests.log)。
六组主机测试覆盖 FM6373C 波形协议、整帧提交、传输所有权、JSON/正文边界、亮度和生命游戏重绘；
使用 Clang AddressSanitizer/UndefinedBehaviorSanitizer。两种固件均通过编译。

本次没有烧录设备，也没有运行会改变设备画面的 BLE 测试。尚需实板确认启动和栈/堆余量、
按键与传图并行、画面和低灰、待机持久化、完整 BLE/HTTP OTA、断线恢复及长时间稳定性。
DMA 时钟与设计帧率来自既有驱动计算，不代表本次测量；旧版 2026-09-13 的实板记录仍保留为历史资料。
