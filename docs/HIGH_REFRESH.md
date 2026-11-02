# 3.2.4 刷新率优先固件

> 后续用户要求提高色深，当前默认已调整为 3.2.5 的 8 bit / 约 303 Hz 配置，见 [提高色深说明](COLOR_RESTORE.md)。下文保留 3.2.4 的高刷选择和历史测试说明，原 4 bit 档现在名为 `pixelpad_s3_refresh_fast`。

按“刷新率优先，接受明显渐变分层”的选择，将默认构建改为 **16 MHz、每通道 4 bit、双缓冲**。已通过原 CH340 串口烧录到同一块 ESP32-S3，10 MHz 和 16 MHz 档均完成启动检查，最终运行默认 16 MHz 档。**以下刷新率均为驱动/时序计算值，尚未用仪器测量。** 本次实板测试详见 [3.2.4 验证记录](HIGH_REFRESH_VALIDATION.md)，不复用旧版结果。

## 可用配置

| 构建环境 | 实际配置时钟 | 颜色位深/通道 | DMA 双缓冲 | 理论扫描率 | 相对旧版 |
| --- | --- | --- | --- | --- | --- |
| 原 3.2.3 | 10 MHz¹ | 8 bit | 否 | 104.2 Hz | 1× |
| `pixelpad_s3_rtos`（默认高刷） | 16 MHz | 4 bit | 是 | 1075.8 Hz | 10.3× |
| `pixelpad_s3_refresh_safe`（原时钟对比档） | 10 MHz | 4 bit | 是 | 672.4 Hz | 6.5× |
| `pixelpad_s3_color`（颜色对比档） | 10 MHz | 8 bit | 否 | 104.2 Hz | 1× |

`pixelpad_s3_ble_only` 采用默认高刷参数，额外关闭 Wi-Fi AP/HTTP OTA。

¹ 固定依赖 HUB75 3.0.15 的 `HZ_8M` 和 `HZ_10M` 枚举值都是 8,000,000，而 ESP32-S3 后端对该档默认使用 `160 MHz / 16 = 10 MHz`。原驱动据 8 MHz 计算并报告约 83 Hz；按其实际配置的时钟重算约 104 Hz，二者都未用示波器测量。新固件通过全局 `S3_LCD_DIV_NUM` 明确分频，同时让 `mxconfig.i2sspeed` 使用相同数值，消除估算偏差。

4 bit 为每通道 16 个编码等级，RGB 编码组合最多 4096 种，CIE 亮度曲线保留。暗部和渐变会明显分层。颜色位深必须通过全局 `PIXEL_COLOR_DEPTH_BITS` 设置，使库内编译的 CIE 查表和 DMA 位平面一致；只调用运行时 setter 会让这版驱动使用错误的查表精度。

这些是屏幕扫描率，不是每秒接收图片数，也不是游戏 FPS。BLE 传输、动画素材帧率和游戏推进周期继续限制内容变化速度。RTOS 负责及时处理绘制任务，实际扫描由 LCD/GDMA 外设完成。[上游驱动说明](https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA/blob/master/doc/memcalc.md)说明了时钟、颜色位深与扫描周期的关系。

## 绘制优化

- 所有 RGB 修改先写入一个 52×52 暂存画面，显示任务在一轮结束后提交最终像素。游戏的清屏和重画不会把清屏过程单独送到前台。
- 两个 DMA 缓冲各自记录待更新像素。换回旧缓冲时补齐跨帧修改，避免连续 `px_patch` 丢失之前的像素。重复设置相同 RGB 可以跳过；没有修改的区域不重新做 CIE 换算和位平面写入，静止画面不反复提交。先清屏再重画的像素仍会标记为修改，但一轮只写入最终颜色。
- 双缓冲在驱动描述符环边界切换。这版 S3 驱动的 flip 是异步的，代码在写回旧前台缓冲前留出两次完整扫描加 100 µs，期间只合并 RGB 更新，不阻塞输入任务。该等待也经过计时回绕测试；用户已确认 16 MHz、10% 亮度下彩条正常且稳定，动态画面的撕裂检查仍待完成。
- 默认配置每像素的位平面写入从 8 次减为 4 次。DMA 帧缓冲和描述符合计约 26,624 字节（双缓冲），旧版约 41,600 字节（单缓冲）；新 RGB 暂存及脏标记另占约 8.8 KB。这里未计容器、分配器等开销，运行时余量以设备统计为准。
- 增加 `display_stats`，区分扫描率估算、提交次数和提交耗时。统计查询不再打断待机动画；原 `rtos_stats` 同样改为纯查询。
- 本次电脑实际协商到 244 字节分包，一张完整图片需要 34 包，暴露出原 32 包 RX 队列在突发写入时溢出的问题。队列扩为 64 包；新增过长包计数和队列峰值统计。最终固件单次彩条传输记录峰值为 34、溢出为零，保留异常时要求重连的保护。

原 52×52 映射、FM6124、时钟相位、锁存消隐和亮度限幅沿用。旧代码记录过“20 MHz”配置下颜色错采样/拖影；该配置在此驱动上实际约 22.86 MHz，因此本次高速候选先到明确的 16 MHz。**16 MHz 已通过启动、基础传图和用户的低亮度彩条目视确认，保留为默认配置；其他画面与长时间稳定性仍需验证，不能宣称已经找到硬件最大值。** `refresh_safe` 名称表示回到原时钟。

## 验证和上板

```sh
CXX=clang++ python tools/test_host.py --sanitize
python tools/display_profiles.py
pio run -e pixelpad_s3_rtos
pio run -e pixelpad_s3_refresh_safe
pio run -e pixelpad_s3_color
```

主机测试覆盖双缓冲交替后的补丁保留、清屏、重复写入合并、边界像素、500 轮随机变更的完整画面对照，以及缓冲复用时间边界和计时回绕；原协议、亮度、生命游戏回归也保留。计算表见 [display-profiles.json](validation/display-profiles.json)。

接回已外接供电的同一块灯板后，先用原时钟档检查颜色、灰阶和单像素棋盘，再切默认 16 MHz 档做同样对比；如出现错色或拖影，回原时钟档。上传不擦除文件系统。

```sh
pio run -e pixelpad_s3_refresh_safe -t upload --upload-port /dev/ttyUSB0
python tools/ble_display_test.py --address AA:BB:CC:DD:EE:01 --pattern bars
python tools/ble_display_test.py --address AA:BB:CC:DD:EE:01 --pattern gradient
python tools/ble_display_test.py --address AA:BB:CC:DD:EE:01 --pattern checker
```

图案工具将当前亮度设为 10%，并替换显示图像，不保存待机动画。每次输出显示统计与 RTOS 堆/栈统计。若要比较物理扫描周期，应观察整圈行扫描/BCM 的重复周期；手机拍摄条纹只适合辅助比较，不能代替精确频率测量。继续进行真实旋钮、游戏、连续传图及欠压复查；此前供电问题仍按原验证记录处理。

默认高刷固件：`.pio/build/pixelpad_s3_rtos/firmware.bin`。原时钟档与颜色档在各自同名构建目录内。修改前 3.2.3 应用镜像和核心显示/运行时文件已保存到 `docs/validation/before-refresh/`；原 `PixelPad_52x52` 工程未改动。
