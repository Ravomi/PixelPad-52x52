# RTOS 3.2.4 高刷验证记录

> 本文为上一版 4 bit 固件的历史记录。后续 3.2.5 提高到 8 bit，当前配置及独立验证见 [提高色深说明](COLOR_RESTORE.md)。

日期：2026-09-14。设备 MAC `AA:BB:CC:DD:EE:00`，ESP32-S3 QFN56 v0.2，16 MB Flash / 8 MB PSRAM，CH340 `/dev/ttyUSB0`；BLE `AA:BB:CC:DD:EE:01`，名称 `PixelPad-XXXX`。最终已烧录默认 **16 MHz、4 bit/通道、双缓冲**固件。

## 构建与烧录

- 默认、BLE 专用、原时钟高刷、颜色对比四个环境均构建成功。最终默认静态 RAM 92,188 / 327,680 字节，Flash 2,448,769 / 3,145,728 字节。
- 串口先烧录 10 MHz 高刷档，启动日志报告 `scan_est=672`。随后烧录默认 16 MHz 档，报告 `scan_est=1075`；两次上传均完成四个写入区段的哈希校验。未擦除文件系统。
- 16 MHz 档实测发现 244 字节分包的突发传图会超过原 32 包 RX 队列。将队列扩大到 64 包后再次编译、烧录，以下“最终固件”测试均在此次修正后完成；原时钟档的上板启动测试发生在队列扩容前，其最终构建另已通过。
- 最终固件大小及 SHA-256 见 [固件清单](validation/firmware-refresh-artifacts.json)；[最终上传/编译日志](validation/upload-refresh-fast.log)、[其他环境编译日志](validation/build-refresh-profiles.log)、[原时钟上传日志](validation/upload-refresh-safe.log)可追溯。
- 原 `PixelPad_52x52` 的迁移输入 320 个文件仍与 SHA-256 清单一致。修改前 3.2.3 应用镜像保存在 `validation/before-refresh/firmware-3.2.3.bin`。

## 主机回归

Clang C++11、`-Wall -Wextra -Werror`、AddressSanitizer + UndefinedBehaviorSanitizer：4 组全部通过，见 [主机测试日志](validation/refresh-host-tests.log)。新增帧缓冲测试用完整参考画面对照 500 轮随机修改，覆盖双缓冲补丁累积、清屏、覆盖写、边界像素和计时回绕；原协议、亮度、生命游戏回归保留。所有 52 行坐标与原映射逐项一致。Python 工具通过语法和图案长度/范围检查。

## 最终固件实板结果

测试使用临时 10% 亮度。用户断开手机 App 后完成电脑 BLE 测试。

| 项目 | 结果 | 证据 |
| --- | --- | --- |
| 244 字节分包传入 8112 字节彩条 | 成功；RX 队列峰值 34，溢出 0 | [首张彩条](validation/refresh-fast-final-ble.log) |
| 同一 BLE 连接连续 50 张完整图像 | 50/50 收到成功 ACK，合计 405,600 字节；RGB level=96 | [连续传图](validation/refresh-fast-images.log) |
| JSON 分片、待机参数边界、超长行恢复 | 全部通过 | [协议测试](validation/refresh-fast-smoke.log) |
| 不完整二进制超时与重新连接 | 无错误成功 ACK，重连后正常 | 同上 |
| 最终恢复彩条并查询状态 | 成功，保持 16 MHz / 4 bit / 双缓冲 | [最终显示统计](validation/refresh-fast-final-bars.log) |
| 16 MHz 彩条目视检查（10% 亮度） | 用户确认“彩条正常，显示稳定”，未反馈错色、横向拖影或明显闪烁 | 2026-09-14 本次会话用户反馈 |

最终统计：`commands=61`，`presented=150`，`uptime_ms=260048`；计数连续，没有在测试过程中重新启动。`rx_overflows=rx_oversize=tx_drops=input_drops=command_drops=0`，RX 队列峰值 34/64。

- 命令在显示队列中的最大等待：1907 µs。
- 一次 RGB 提交的最大 CPU 耗时：3513 µs；显示任务单轮最大处理时间：5708 µs。
- 最终可用堆：67,020 字节；历史最低：57,744 字节。
- input/render/protocol/service 历史最小剩余栈：2380 / 8612 / 5244 / 3460 字节。
- 这轮 `input_events=0`，没有用实际旋钮完成输入延迟验收。

连续传图连接的 Bleak 公共接口报告分包上限仍为 **20 字节**；50 张图片的接收 ACK 往返中位数 2740.161 ms，P95 3100.342 ms，最大 3144.781 ms。单独彩条连接曾报告 **244 字节**并完成突发传输，两种分包情形都进行了验证。不同连接的协商/主机行为有差异，不能把扫描率提升当作 BLE 传图提速；图案工具的 `send_seconds` 只统计写入主机调用的时间，不是空中传输完成时间。

最终固件启动后的 60 秒 UART 记录没有新增 BROWNOUT、panic、watchdog 或异常复位，见 [最终串口记录](validation/refresh-fast-final-uart.log)。开头的 POWERON 是打开串口触发的预期复位；仍有原先的无 core dump 分区、未存 BLE 名称、无待机文件提示。原时钟档 35 秒及首轮 16 MHz 档 50 秒记录同样无新增异常复位。

## 验证边界

`1075 Hz` 是设备驱动的整数扫描率估算；按描述符时序计算约 1075.8 Hz，旧配置约 104.2 Hz，理论约 10.3 倍。**尚未用示波器/光电传感器测量物理刷新率，游戏和 BLE 内容帧率不等于这个数值。**

用户已确认 16 MHz 档在 10% 亮度下七列彩条正常、显示稳定，保留当前高刷配置。此项目视检查不等于物理刷新率测量。未完成满亮度供电、长期游戏运行和所有画面模式的视觉验收；本次短时无欠压不代表此前供电问题已根治。当前设备保持 10% 亮度和彩条画面；原时钟高刷构建保留。
