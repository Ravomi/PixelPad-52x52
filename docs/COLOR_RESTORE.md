# RTOS 3.2.5：提高色深，保留高刷

用户在确认 3.2.4 的 4 bit 高刷彩条正常后，希望增加色深、尽可能还原颜色。默认配置改为 **16 MHz、每通道 8 bit、双缓冲**，最低扫描目标 240 Hz，固定驱动计算结果约 **303.4 Hz**。这是扫描率估算，不是仪器测量，也不是内容 FPS。

## 配置选择

在相同 16 MHz、双缓冲、最低 240 Hz 配置下：

| 色深/通道 | 理论扫描率 | DMA 缓冲和描述符估算 |
| --- | --- | --- |
| 4 bit（上一版） | 1075.8 Hz | 26,624 字节 |
| 6 bit | 319.8 Hz | 52,416 字节 |
| 7 bit | 311.4 Hz | 57,824 字节 |
| 8 bit（当前默认） | 303.4 Hz | 63,232 字节 |

8 bit 相比 6 bit 少约 5% 扫描率，却有更多颜色编码精度，因此本次选 8 bit。相对于最初 3.2.3 的约 104.2 Hz，当前扫描率估算仍约为 **2.9 倍**；相对于上一版 4 bit 的 1075.8 Hz 则主动降低，以换取颜色细节。

`PIXEL_COLOR_DEPTH_BITS=8` 全局作用于应用和驱动，保证 8 位平面与 8 位 CIE 亮度查表匹配。每通道编码从 16 个值增加到 256 个值，预期减轻渐变和暗部的粗分层。驱动为了满足最低扫描率选择 transition bit 2，实际可辨灰阶仍受 BCM 时间分配、面板和总亮度影响；这里不是色彩校准或“真实显示全部 1677 万色”的保证。

## 内存和绘制

保留 3.2.4 的帧提交、双缓冲独立脏标记及 64 包 BLE RX 队列。CPU 写入的 RGB 暂存画面及脏标记约 8.8 KB，改为启动时明确分配到 PSRAM；DMA 位平面及描述符继续位于内部 RAM，保持已通过目视检查的 16 MHz 输出路径。

RGB 暂存分配失败会在启动时明确报错，不将空指针交给绘制任务。双缓冲复用等待继续根据当前扫描率计算：303 Hz 时至少等待 6702 µs 后才重写旧前台缓冲；期间只合并暂存画面更新。计时边界已纳入原帧缓冲回归。

## 构建档位

| 环境 | 参数 | 理论扫描率 |
| --- | --- | --- |
| `pixelpad_s3_rtos` | 16 MHz / 8 bit / 双缓冲 | 303.4 Hz |
| `pixelpad_s3_ble_only` | 同上，关闭 Wi-Fi AP / HTTP OTA | 303.4 Hz |
| `pixelpad_s3_refresh_fast` | 上一版的 16 MHz / 4 bit / 双缓冲 | 1075.8 Hz |
| `pixelpad_s3_refresh_safe` | 10 MHz / 4 bit / 双缓冲 | 672.4 Hz |
| `pixelpad_s3_color` | 原扫描/色深配置，10 MHz / 8 bit / 单缓冲 | 104.2 Hz |

配置计算结果见 [color-restore-profiles.json](validation/color-restore-profiles.json)。修改前的 3.2.4 默认应用镜像保存在 `validation/before-color-restore/firmware-3.2.4.bin`，原工程仍保持不变。

测试图采用红、绿、蓝、灰四个横带，每带从左侧黑色渐变到右侧最大值，用于检查渐变层次，而不只比较满色彩条。测试维持 10% 亮度。

## 当前实板验证（2026-09-14）

讨论 USB-TTL 的 TX/RX 接线后，用户要求再次烧录，本次 ROM 握手成功。读到 ESP32-S3 QFN56 v0.2、16 MB Flash、8 MB 内置 PSRAM；MAC 为 `AA:BB:CC:DD:EE:00`，与之前记录相同。以下结果对应本次实际连接的芯片，不据此推断另一块板的状态，也未确认此前失败一定由 TX/RX 接反造成。

- 默认环境重新上传成功，四个写入区段均通过哈希校验，见 [成功烧录日志](validation/upload-color-restore-connected.log)。没有执行整片擦除或写入文件系统。应用镜像 SHA256 仍为 `96434873b7268944dc50305692685c3ec4a38af6b2d52c9a00af40e92b483a85`，与已构建的 3.2.5 一致。
- [60 秒启动串口记录](validation/color-restore-connected-uart.log)确认版本 3.2.5，`clock=16000000`、`depth=8`、`double_buffer=1`、`scan_est=303`，任务启动后 heap 43,512 字节、PSRAM 8,289,899 字节。只有工具触发的启动复位，未见新增欠压、panic 或 watchdog。仍有未配置 core dump 分区、未保存 BLE 名称及无待机文件提示。
- 扫描到 `PixelPad-XXXX` / `AA:BB:CC:DD:EE:01`，设备识别服务匹配，见 [扫描记录](validation/color-restore-connected-ble-scan.json)。10% 亮度红、绿、蓝、灰渐变图 8,112 字节发送并收到 ACK；244 字节分包连接上队列峰值 15，未溢出，见 [首次渐变传图](validation/color-restore-connected-gradient.log)。
- BLE 五项基础检查全部通过：分片头、待机参数边界、超长行恢复、二进制接收超时、断开重连；见 [BLE 检查日志](validation/color-restore-connected-smoke.log)。
- 同一 BLE 连接连续传入 10 张完整图片，共 81,120 字节，全部收到 ACK。本次该连接协商为 20 字节分包，单张耗时中位数 2582.682 ms、最大 2919.565 ms；这是主机到 ACK 的传输时间，不是屏幕刷新周期，见 [连续传图日志](validation/color-restore-connected-images.log)。
- 测试结束恢复 10% 亮度红、绿、蓝、灰渐变图，见 [最终显示和运行统计](validation/color-restore-connected-final-gradient.log)。运行时间连续增至 162,517 ms；RX 溢出、超长包、TX 丢弃、输入丢弃和命令丢弃均为 0，RX 队列峰值 30；heap 39,684 字节、最低 heap 29,140 字节，各任务剩余栈为 2380 / 8644 / 5244 / 3460 字节。CPU 帧提交最大耗时 5208 µs、渲染迭代最大耗时 7421 µs。串口及 BLE 连接已关闭，电脑蓝牙恢复原先的关闭状态，灯板保留渐变图。

以上是短时、低亮度实板检查。实际光学扫描率仍未用仪器测量，渐变层次和错色/闪烁仍需用户目视确认；未做满亮度或长期运行验收。

## 已完成的构建与此前连接排查

以下失败记录保留排查过程；串口通信和本次烧录现已成功，当前状态以上节为准。

- 五个环境全部构建通过，默认固件静态 RAM 83,388 字节，Flash 2,440,061 字节。见 [默认构建日志](validation/build-color-restore.log)、[其他环境构建日志](validation/build-color-restore-profiles.log)及 [固件哈希清单](validation/firmware-color-restore-artifacts.json)。
- 4 组 Clang ASan/UBSan 主机回归通过，含新增 303 Hz 双缓冲复用边界，见 [主机日志](validation/color-restore-host-tests.log)。
- [一次串口上传](validation/upload-color-restore.log)记录目标仍为旧板 MAC `AA:BB:CC:DD:EE:00`，四个写入区段均完成哈希校验。该结果不能证明用户后来更换的新板已烧录。
- 用户随后告知已更换板子，并重新上电。电脑能识别 CH340 `/dev/ttyUSB0`，但新板的 ROM 下载握手仍报 `No serial data received`；一次重上电期间的连接报串口数据损坏。两次 60 秒 UART 捕获没有数据，不能用空日志宣称运行稳定。
- 用户确认新板没有 BOOT/RESET 按钮，并要求强制烧录。已尝试无复位握手、延长 DTR/RTS 原子复位、持续保持 BOOT 信号，以及 9600 / 115200 波特率同步，均未建立连接；见 [强制下载尝试](validation/new-board-force-boot.log)。另尝试不复位连接可能遗留的 460800 / 921600 波特率下载器，仍失败。蓝牙扫描也未找到 PixelPad 名称或设备识别服务。
- 新板尚未识别出芯片/Flash/MAC，**没有向新板成功写入固件**。已释放 DTR/RTS、关闭串口并恢复电脑蓝牙为原先的关闭状态。需要确认新板型号及下载/复位焊盘接线后继续。新板运行状态、内存余量、渐变目视效果和实际刷新率均未完成验证。
- 用户再次要求重试后，重新识别到 CH340（USB 设备编号已变化），先以 115200 尝试 ROM/Flash 识别，再执行完整 `pio run -e pixelpad_s3_rtos -t upload --upload-port /dev/ttyUSB0`。仍在连接阶段报 `Serial data stream stopped: Possible serial noise or corruption`，没有进入擦除或写入阶段；见 [再次烧录日志](validation/upload-color-restore-retry.log)。
- 用户补充已连接 USB-TTL 后，仍只识别到同一 CH340 `/dev/ttyUSB0`。115200 无复位握手无应答；临时关闭 Linux `HUPCL` 并重试自动复位后仍报串口数据中断。释放 DTR/RTS 后监听 5 秒仅收到一个 `0x00`，不构成有效 ROM 或固件日志。已恢复串口原设置并关闭端口，见 [USB-TTL 探测日志](validation/new-board-usb-ttl-probe.log)。下一步需要实际 TX/RX/GND 与 IO0/EN/DTR/RTS 接线信息；USB 转换器枚举成功不能证明 ESP32-S3 已进入下载模式。
