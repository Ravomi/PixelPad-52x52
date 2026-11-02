# ESP32-S3 RTOS 选择与响应速度评审

> 本文评审最初的 RTOS 迁移。随后按刷新率优先要求增加了 3.2.4 高刷配置，当前扫描参数及验证状态以 [高刷说明](HIGH_REFRESH.md) 为准。

评审日期：2026-09-14。对象是 `PixelPad_52x52` 当前工作区的 52×52 ESP32-S3 固件。源码仓库 HEAD 为 `95dbc14a4adeaace052fbd0bfee55817fe99e12c`，但迁移输入还包含未提交的工作区内容，以同目录 SHA-256 清单为准。

## 结论

**选择 ESP-IDF FreeRTOS，先保留 Arduino 驱动兼容层并重构任务。** 原 Arduino 程序已经运行在 FreeRTOS 的 `loopTask` 中，并不是裸机程序。此次改造的核心是业务并发和阻塞隔离，不是安装一个内核就获得加速。Arduino 2.0.17 的入口源码直接创建 FreeRTOS 任务，可见 [官方源码](https://github.com/espressif/arduino-esp32/blob/2.0.17/cores/esp32/main.cpp)。

| 方案 | 对本工程的适配 | 迁移投入判断 | 结论 |
| --- | --- | --- | --- |
| ESP-IDF FreeRTOS + Arduino 兼容层 | 可继续使用 BLEDevice、HUB75 DMA、OneButton、ESP32Encoder、LittleFS 和现有 OTA | 最低，主要改变任务和数据所有权 | 本次采用 |
| 原生 ESP-IDF + FreeRTOS | 仍使用同一内核体系；可进一步控制组件、构建配置、内存和调试 | 需要逐项替换/封装 Arduino 依赖，或将 Arduino 作为 IDF 组件 | 后续演进方向，不是本次已经完成的工作 |
| Zephyr | 官方有 ESP32-S3-DevKitC 支持；跨厂商平台的统一接口有价值 | 当前应用大量依赖 Arduino/ESP-IDF 接口，需要适配并验证定制显示驱动、BLE、OTA | 若以后有明确跨芯片需求再评估 |
| RT-Thread | 需要重新建立本工程的 BSP/驱动/构建验证基础 | 当前目录没有相关适配；本次未完成 S3 外设覆盖和现有显示库兼容验证 | 不因“提速”这个单一目标引入 |

上表的投入和适合程度是根据本工程依赖作出的工程判断，不是三个 RTOS 的实测性能排名，也不表示 Zephyr 或 RT-Thread 内核本身更慢。

ESP-IDF 对 ESP32-S3 提供双核 FreeRTOS、核亲和性和队列/任务机制，详见与本次 Arduino 基线匹配的 [ESP-IDF 4.4.7 FreeRTOS 文档](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/system/freertos.html)。Zephyr 的芯片支持以 [官方 ESP32-S3 板级文档](https://docs.zephyrproject.org/latest/boards/espressif/esp32s3_devkitc/doc/index.html) 为依据。

本次锁定 PlatformIO espressif32 6.13.0、Arduino 包 3.20017.0（Arduino 2.0.17），目的是保持与原驱动兼容；这不代表它是未来新产品应长期采用的最新 SDK 基线。后续若转为原生 ESP-IDF 工程，按当时 [Arduino 作为 ESP-IDF 组件的版本兼容说明](https://docs.espressif.com/projects/arduino-esp32/en/latest/esp-idf_component.html) 选择组合，另行验证 HUB75 和 OTA。

## 源码发现与处理

以下“确认”仅表示由源码确认，不代表已实测对应延迟。

| 发现 | 原代码证据 | 影响 | 本次处理 |
| --- | --- | --- | --- |
| 图片接收阻塞主循环，超时参数 5000 ms（确认） | 原 `src/main.cpp` 调用 `readLargeData`，按钮处理位于同一 loop | 接收期间按键业务无法继续运行 | 协议任务接收，显示任务独立；完整帧后才更新画面 |
| 自定义待机接收超时参数 300000 ms（确认） | 原 `src/main.cpp` 的 `set_standby` | 长传输或发送中断可长期占住业务循环 | 协议任务接收/持久化，PSRAM 指针所有权移交显示任务 |
| BLE RX 2048 字节环形缓冲区满后直接忽略字节（确认） | 原 `lib/Bluetooth/Bluetooth.cpp` 的 `RxCallback::onWrite` | 图片数据可能损坏；volatile 索引也不是完整跨核同步机制 | 32×512 字节容量的 FreeRTOS 包队列；整包入队，溢出计数并使会话失效 |
| BLE 断连回调 delay(500)（确认） | 原 Bluetooth `onDisconnect` | 占住蓝牙回调执行上下文 | 后台任务到期重启广播 |
| OTA 注释声称回调轻量，但升级请求含 Wi-Fi 操作、Update.begin、delay(50/100)（确认） | 原 `BleOTA::handleUpgradeRequest` 直接由 onWrite 调用 | OTA 开始时阻塞 BLE host，且状态分散在两个任务 | BLE 回调只投递原始包，OTA 工作任务独占协议状态、Flash 和 NVS |
| 吃豆人碰撞 delay(500)（确认） | 原 `PacmanGame::updateGhost` | 整个业务循环停顿 | 500 ms 死亡状态计时，循环立即返回 |
| 生命游戏编辑模式每次 loop 都全屏绘制（确认） | 原 `GameOfLife::loop` | 静止画面浪费 CPU 时间 | 输入时立即重绘；无输入时只在 200 ms 光标相位变化时重绘 |
| HTTP raw OTA 首块未写入，完成回调还 delay(2000)（确认） | 原 `OTA.cpp` 在 index==0 分支直接 return | 固件正文缺头；AsyncTCP 上下文被延时占用 | 写入首块、检查连续偏移/完整长度、断连清理，后台延后重启 |

主循环、RX、输入、游戏和 OTA 的对应迁移文件都位于新目录；原工程保持原样。

## 为什么响应会改善

以前图片/动画接收、Flash 持久化、输入和绘制串行执行。即使 BLE 数据到达慢，业务循环也只能等待。现在接收任务等待队列时会阻塞自身，输入任务继续周期采样，显示任务继续处理输入和更新当前画面。

同样重要的是所有权：LED、App 和游戏状态只在初始化阶段及之后的显示任务中修改；采样任务只发输入事件，协议任务只把已准备好的命令和数据交给显示任务。NUS TX 由一个后台任务发送，避免多个业务任务把 `setValue` 和 `notify` 交错执行。BLE OTA 的状态与 Flash 操作也统一到其工作任务。

接收队列为缓冲突发数据提供余量，不能无限提高链路容量。持续超出消费速度时会报错，不能依靠更大的队列解决长期过载。

## 能改善和不能直接改善的指标

- 能从结构上隔离：网络等待造成的按键停顿、接收时应用循环不执行、回调里的显式延时、静止画面重复渲染。
- 初次迁移没有改变 BLE PHY、主机协商连接间隔/MTU、52×52 面板的扫描时钟。原配置枚举为 8 MHz，后续核对固定版本的 S3 驱动发现实际配置输出为 10 MHz；3.2.4 另行提供高刷档，详见上方说明。
- 不等于硬实时：跨核任务仍共享内存/Flash；Flash 操作会涉及缓存与系统临界区。队列等待上界还受渲染耗时、蓝牙拥塞和系统负载影响。
- 2 ms 是按键采样配置。OneButton 的消抖、区分单击/双击、长按判定和旋钮 35 ms 合并周期仍会影响用户感受，BLE TX 和手机 UI 还会增加延迟。
- 成本：本工程五个显式应用任务申请约 40 KiB 栈，另有 Arduino/系统任务；RX、TX、命令和 OTA 队列也占内存。构建报告的静态 RAM 不包含所有运行时申请，必须上板测堆/栈余量。

## 验收与后续

本次完成独立 PlatformIO 工程和 FreeRTOS 任务重构；未将全部驱动改写为原生 ESP-IDF 组件，已开始实板验证，结果与未覆盖项目见 [VALIDATION.md](VALIDATION.md)。

建议使用同一块板、同一客户端、相同 MTU/发送节奏，对比旧版和迁移版。下列是验收目标，不是已达成的性能数据：

1. 图片和待机上传中持续操作旋钮、单击、长按；确认采样不中断，图片接收结束前可收到输入事件，既有 App 按顺序发送正文，不因事件插入新命令。
2. 正常负载 `input_drops`、`command_drops`、`rx_overflows`、`tx_drops` 为 0；输入事件入队到显示任务处理的最大延迟目标 <20 ms，具体以任务统计和硬件测量为准。
3. 连续图片上传至少 100 次，记录传输+ACK 的中位数/P95，与旧版相同条件比较。物理“按下到发光”延迟需高速摄影或 GPIO/逻辑分析仪单独测量。
4. 游戏运行、待机写 Flash 和 BLE 流量同时存在时，观察 watchdog、内部堆最低值和每个任务栈余量；当前栈大小是初始预算，未经过压力测试校准。
5. 覆盖超长命令、分片换行、二进制包含换行/零字节、故意溢出、接收超时、断连重连，确认不 ACK 损坏帧、不把旧连接数据交给新会话。
6. 待机内容重启恢复、存储不足/错误、文件截断和上传中断；原内容不应因一次接收失败被释放。媒体无 CRC 的既有协议仍无法识别所有发送端字节流混入错误。
7. BLE OTA、ElegantOTA、raw OTA 分别完整升级、错误包/CRC、断连、重连、首次启动与回滚测试。一次只测试一个升级通道；跨通道并发还需专门设计仲裁并验收。

如果实测瓶颈仍主要在手机发送间隔和 BLE 空中链路，下一步应调整 App 的分包、连接优先级和 ACK/流控协议。如果主要在绘制，则进一步评估脏区域、帧缓冲和 DMA 带宽。这些工作都可以继续基于 FreeRTOS 完成。
