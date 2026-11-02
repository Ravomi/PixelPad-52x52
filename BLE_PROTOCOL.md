# PixelPad BLE 通信协议文档

## 概述

所有通信通过蓝牙 **Nordic UART Service (NUS)** 进行。

| UUID | 用途 |
|------|------|
| `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | UART 服务 |
| `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | RX 特征值（写入 — 接收命令和数据） |
| `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | TX 特征值（通知 — 发送响应） |

协议为 **JSON 行协议**：每个命令以 `\n` 结尾的 JSON 字符串通过 RX 特征值发送。

---

## 亮度调节

### 命令格式

```json
{"cmd":"bright","val":<0-100>}
```

### 参数

| 字段 | 类型 | 范围 | 说明 |
|------|------|------|------|
| `cmd` | String | `"bright"` | 命令标识符 |
| `val` | Int | 0–100 | 亮度百分比（0=关闭，100=最亮） |

### 处理流程

1. App 端发送 JSON 命令至 RX 特征值
2. 设备端接收后从 `"val"` 字段提取 0–100 的整数值
3. 内部映射：`mapped = (val * 255) / 100`（转换为 0–255）
4. 调用 `ledDisplay.setBrightness(mapped)`，固件在驱动入口统一计算 `floor(mapped × 90 / 100)` 后设置硬件 PWM 占空比（3.0.3 起，最大为 229/255，约为原来的 90%；开机和旋钮调节同样适用）

### 响应

无显式响应。

---

## 待机动画协议

### 命令列表

| 命令 | 用途 |
|------|------|
| `{"cmd":"standby"}` | 强制触发待机动画（调试用） |
| `{"cmd":"clear_standby"}` | 清除自定义待机动画 |
| `{"cmd":"set_standby", ...}` | 设置自定义待机动画（3 步握手） |
| `{"cmd":"mode", ...}` | 切换应用模式 |

### 自动待机

60 秒无操作后，设备自动进入待机模式，播放内置 GIF 待机动画（92 帧，52×52 像素）。

### `set_standby` — 自定义待机动画传输协议

采用 **3 步握手** 协议：

#### 第 1 步：客户端发送元数据

```json
{"cmd":"set_standby", "frames":<N>, "fps":<N>, "size":<totalBytes>}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `frames` | Int | 动画帧数 |
| `fps` | Int | 帧率（默认 8） |
| `size` | Int | 数据总字节数 = `frames × 52 × 52 × 3` |

#### 第 2 步：设备返回 `ready`

```json
{"status":"ready","cmd":"set_standby"}
```

#### 第 3 步：客户端发送原始 RGB 数据

通过 RX 特征值连续写入原始二进制数据（无分包协议）。

- **分辨率**：52 × 52 像素
- **像素格式**：RGB（每像素 3 字节：R, G, B）
- **单帧大小**：52 × 52 × 3 = **8,112 字节**
- **总数据量**：`frames × 8,112` 字节
- **超时**：300 秒
- **数据顺序**：逐行扫描（先从左到右，再从上到下），每行按 R, G, B 顺序

#### 第 4 步：设备返回结果

```json
{"status":"ok","cmd":"set_standby"}
// 或
{"status":"err","cmd":"set_standby","reason":"<reason>"}
```

错误原因：`invalid_json`、`invalid_params`、`out_of_memory`、`recv_fail`

### 清除自定义待机

```json
{"cmd":"clear_standby"}
```

响应：

```json
{"status":"ok","cmd":"clear_standby"}
```

### 持久化存储

自定义待机数据存入 LittleFS 文件 `/standby.bin`：

| 偏移 | 大小 | 说明 |
|------|------|------|
| 0 | 4 字节 | 魔数 `0x53544259` (`'STBY'`) |
| 4 | 4 字节 | 帧数 (int32) |
| 8 | 4 字节 | FPS (int32) |
| 12 | `frames × 8112` | 原始 RGB 帧数据 |

设备启动时自动从 LittleFS 加载到 PSRAM。

### 数据流示意

```
App (手机)                          PixelPad (设备)
    │                                      │
    │  {"cmd":"set_standby","frames":3,     │
    │   "fps":8,"size":24336}              │
    │─────────────────────────────────────>│
    │                                      │
    │  {"status":"ready","cmd":"set_standby"}│
    │<─────────────────────────────────────│
    │                                      │
    │  [原始 RGB 数据, 24336 字节]           │
    │  ── 连续 BLE 写入 ────────────────>  │
    │  (无需分包/ACK 协议)                  │
    │                                      │
    │  {"status":"ok","cmd":"set_standby"}  │
    │<─────────────────────────────────────│
    │                                      │
    │  [设备存储到 LittleFS /standby.bin]  │
```

---

## 图片接收协议

### 命令格式

```json
{"cmd":"recv","size":<totalBytes>}
```

- `size` = 52 × 52 × 3 = **8,112 字节**（固定）

### 流程

1. 客户端发送 `{"cmd":"recv","size":8112}`
2. 设备返回 `ready`
3. 客户端发送 8,112 字节原始 RGB 数据
4. 设备接收后直接显示，超时 5 秒
5. 响应：

```json
{"status":"ok","cmd":"recv"}
// 或
{"status":"err","cmd":"recv","reason":"<reason>"}
```

---

## 模式切换

```json
{"cmd":"mode","mode":"<mode>"}
```

| mode 值 | 说明 |
|---------|------|
| `"image"` | 图片显示模式（默认） |
| `"life"` | 康威生命游戏 |
| `"pacman"` | 吃豆人游戏 |
| `"sand"` | 流沙俄罗斯方块 |

切换时播放 3D 旋转转场动画。

---

## 按键指令

```json
{"cmd":"key","key":"<key>"}
```

| key 值 | 说明 |
|--------|------|
| `"UP"` | 上 |
| `"DOWN"` | 下 |
| `"LEFT"` | 左 |
| `"RIGHT"` | 右 |
| `"A"` | A 键 |
| `"B"` | B 键 |

---

## 事件通知（TX → App）

设备通过 TX 特征值向 App 发送以下 JSON（以 `\n` 结尾）：

| 事件 | 说明 |
|------|------|
| `{"evt":"color_plus"}\n` | 旋钮顺时针旋转（切换色号 +1） |
| `{"evt":"color_minus"}\n` | 旋钮逆时针旋转（切换色号 -1） |
| `{"evt":"click"}\n` | 单击 |
| `{"evt":"dbl_click"}\n` | 双击 |
| `{"evt":"all_white_on"}\n` | 长按开始，设备进入全白显示 |
| `{"evt":"all_white_off"}\n` | 长按结束，App 可重新下发当前画面 |

---

## 注意事项

1. **设备名**：默认 `"PixelPad"`，可通过 BLE 自定义（最长 20 字符，仅允许字母数字、下划线、短横线）
2. **BLE MTU**：依赖 MTU 大小（默认 23 字节，可通过 MTU Request 协商提升），大数据传输时设备使用 2048 字节环缓冲区和 300 秒超时
3. **OTA**：OTA 升级期间所有命令（包括亮度调节）会被忽略
4. **所有命令都会重置待机计时器**（60 秒无指令进入待机）

## RTOS 3.2.3 补充（本工程以此节覆盖上文旧实现描述）

- NUS、型号识别和 OTA UUID 保持不变；设备默认名称为 MAC 后缀形式 `PixelPad-XXXX`，可通过 `ble_name` 保存。
- 每条 JSON 最多 1023 字节，不含换行。接收器支持跨 ATT 写分片；超长行整行丢弃，返回 `line_too_long`，不执行截断内容或尾部片段。
- RX 采用 32 个最大 512 字节的包队列，替代 2048 字节环缓冲。服务端 MTU 上限为 517，实际 MTU 由客户端协商；单个特征值仍最多 512 字节。
- 图片 `recv` 仍不需要 READY 握手。接收 8112 字节 RGB，5 秒总超时，成功返回 `{"status":"ok","cmd":"recv"}`。接收中继续显示上一帧，完整接收后绘制。
- `set_standby` 必须满足 1–256 帧、1–60 FPS（未提供 fps 时为 8）、`size == frames*8112`；总接收超时 300 秒。分配成功才回复 READY，写入 LittleFS 成功且显示任务接管数据后回复 OK。除原错误外，可能返回 `storage_fail`、`busy`。
- 任一二进制接收失败、`recv` 参数错误/分配失败，或 RX 包队列溢出后，当前接收会话失效。收到 `recv_fail` / `rx_desync_reconnect` 后断开重连，再发送完整请求；不能在残余二进制流中直接重试 JSON。队列满时不再静默丢掉个别像素字节。
- 同一 RX 通道仍需串行写入：JSON header、完整二进制正文、等待 ACK，然后开始下一条请求。正文中不可插入旋钮触发的图片更新或其他 JSON。
- `px_patch` 支持带空白的合法 JSON；退出动画后应用补丁，后续补丁保留未修改像素。支持原 `game`/`key` 形式和 `{"cmd":"key","key":"UP"}`。
- 保留 35 ms 旋钮事件合并节奏及 OneButton 默认手势判定。2 ms 采样周期不等于单击通知在 2 ms 内到达手机。
- 空闲画面不自动启动屏保；待机播放由 `standby` 显式触发，与迁移源当前行为一致。
- BLE OTA 回调仅排队；Flash、NVS、CRC、升级控制和 OTA 通知归 `pp_ota` 所有。原始 OTA 包队列溢出会中止会话并返回写入错误，需重连重新开始。

### `rtos_stats`

请求（末尾追加 `\n`）：

```json
{"cmd":"rtos_stats"}
```

回复字段：

| 字段 | 含义 |
| --- | --- |
| `input_events` / `commands` | 显示任务已处理的输入事件/命令总数 |
| `input_max_us` | 从输入事件入队到显示任务开始处理的最大间隔，微秒 |
| `command_max_us` | 从准备好的命令入队到显示任务开始处理的最大间隔，微秒；不包含接收和文件写入 |
| `render_max_us` | 一轮显示任务（输入、命令、动画/游戏）的最大处理耗时，微秒；不含末尾主动休眠 |
| `rx_overflows` | RX 包过长或接收队列满的累计次数 |
| `tx_drops` | NUS 通知过长或发送队列满的累计次数；不是空中丢包统计 |
| `input_drops` / `command_drops` | 输入/命令队列投递失败累计次数 |
| `heap` / `min_heap` | Arduino `ESP.getFreeHeap()` / `getMinFreeHeap()` 返回值，字节 |
| `stack_free` | ESP-IDF 栈历史最小剩余量，字节；顺序 input、render、protocol、service |

计数和最大值在重启后清零。该查询不包含 GPIO 硬件边沿、消抖/双击等待、无线传输、手机 UI 或屏幕扫描时序；端到端延迟需要另行测量。较长 JSON 通知按当前 ATT MTU 分片，客户端需累计到换行再解析。

### 3.2.4 高刷补充：`display_stats`

请求 `{"cmd":"display_stats"}`，末尾追加换行。回复示例中的速率为驱动计算值：

```json
{"status":"ok","cmd":"display_stats","clock_hz":16000000,"depth_bits":4,"double_buffer":true,"scan_hz_est":1075,"presented":10,"pixel_writes":27040,"present_max_us":2000,"uptime_ms":10000}
```

- `clock_hz`：配置的 LCD 输出时钟；`depth_bits`：每通道位平面数；`double_buffer`：是否双缓冲。
- `scan_hz_est`：驱动根据时钟/尺寸/位平面计算的扫描 Hz，整数截断，**不是仪器测量值，也不是内容 FPS**。
- `presented`：启动以来 RGB 帧提交次数；双缓冲时计切换请求。静态画面持续由硬件扫描，此计数不增加。两次采样的差值除以采样时间可估算内容提交速率。
- `pixel_writes`：实际向 DMA 缓冲写入的累计像素数，包括旧缓冲需要补齐的像素。
- `present_max_us`：一次 RGB 暂存画面提交至 DMA 的最大 CPU 耗时；不包含帧在屏幕上扫描完成的等待。
- `uptime_ms`：设备 `millis()`，用于采样间隔；计数按无符号 32 位处理，重启清零。

`recv` 成功 ACK 表示完整图像已接收并放入 RGB 暂存画面，显示任务在本轮末尾统一提交。如果一轮有多次写入，只提交最终画面。它不保证 ACK 到达时面板已扫描完成。`px_patch` 的未修改像素在双缓冲之间持续保留。

`rtos_stats` 与 `display_stats` 均为纯查询，不再触发 `App::updateActivity()`，因此可以在待机动画播放期间采样。`render_max_us` 现在也包含本轮的画面提交耗时。

3.2.4 的 RX 队列从 32 扩至 64 个包，单包仍最多 512 字节，以容纳 244 字节分包时完整图片的 34 包突发。`rtos_stats` 新增 `rx_oversize`（超过单包上限的次数，包含在 `rx_overflows` 内）和 `rx_queue_peak`（观测到的排队包数峰值）。达到容量上限仍会使 RX 会话失效并要求重连，不静默丢包。


## FM6124 传输互斥与接收期限

BLE OTA、HTTP OTA 和内容/待机文件写入共享互斥所有权；
OTA 期间暂停业务命令与输入处理。设备名称写入后保留内容所有权直到重启。
BLE OTA 失败清理时恢复之前的 Wi-Fi 模式。HTTP OTA 只接受应用固件。
`recv` 保持头部后直接发送正文的协议，不新增 READY；接收空闲超时 5 秒，
总期限 30 秒，因此持续推进的小包传输不会在 5 秒时被无条件中断。
待机上传的空闲/总期限均为 300 秒。主机测试不等同于并发 OTA 的实板验收。


## FM6124 本地渐变显示

`recv` 可带 `fade_ms`（0～500 毫秒，默认 0）。完整接收后在设备上做 smoothstep
渐变，中途收到新目标则从最后提交给 DMA 的 RGB 画面继续过渡。ACK 表示目标已提交，
不表示渐变已经完成。原 FM6124 引脚、上下半屏映射、扫描参数和亮度策略保留。
`display_stats` 增加 `driver:fm6124`、`committed`、`shown`、`shown_hash`、
`fading`、`transition_frames`。`shown_hash` 为最近提交给 DMA 的 RGB888 数据的
FNV-1a 32 位哈希；双缓冲切换是异步的，这些计数不代表光学测量。


## FM6124 3.2.6 当前图纸驻留

`capabilities` 返回 `rotary_frames_v1:true,max_regions:64`。

- `rotary_frame_begin`：`session`、`order`（非负 region ID 数组，1～64 个唯一 ID）、`wrap`。
- `rotary_frame_store`：`session`、`region`、`size`。`region:-1` 是完整底图 8112 字节；其他 ID 是 16224 字节的色号图 + 区域图。必须等 `status:ready` 再发送正文，等 `status:ok` 才算缓存成功。
- `rotary_frame_select`：`session`、`region`、`code`、`hold_ms`（0～2000）、`fade_ms`（0～500）、`after_input`。全量缓存就绪且序号同步后启用本地旋钮；`region:-1` 显示底图并保留未选择的初始规则。
- `rotary_frame_clear`：仅清理匹配 `session` 的缓存。
- `rotary_stats`：查询 session、loaded、count、armed、region、local_steps。

本地旋钮通知仍是原有 `evt`，附加非零 `rotary_session` 与 `rotary_region`。匹配当前图纸时，App 采用通知中的 region 更新游标并停止回传画面；不同图纸的旧通知不能作为新图纸的步进。


## FM6124 3.2.6 旋钮队列和连续接收

能力特征 F6 新增 `streamRecv:1`：`recv` 头部写入完成后可立即发送二进制正文，不必固定等待 50 ms。App 必须读取该能力，旧设备仍保留原等待方式；正文大小、串行写入、最终 ACK 和接收超时规则不变。`frameFade:1` / `fade_ms:200` 仍有效。

每个完整旋钮步进继续发送一条原有 `evt` 通知，增加可选字段，旧 App 可忽略：

```json
{"evt":"color_plus","seq":42,"pending":3}
```

`seq` 是本次启动内成功入运行时队列的输入序号（无符号 32 位，允许回绕），包含按钮事件。`pending` 是当前事件之后尚在运行时 FIFO 的条数，不包含采样器积压和已交给 BLE TX 的条数。不要跨连接补播事件；重新连接时未发送的旧输入会清除。

```json
{"cmd":"input_stats"}
```

回执提供 `events`、`last_seq`、`pending`、`queue_peak`、`backlog`、`backlog_peak`、`backlog_full`、`queue_retries`、`tx_retries`、`button_drops`、`max_us`。`max_us` 从运行时入队到 TX 入队，不包含采样器积压或手机处理。重试数不等于丢步数；输入 TX 保留四个回执槽，防止快速转动挤掉图片 ACK。


能力特征完整 UUID 为 `7DA35D3C-AD91-41BA-8A6B-D375CC1116F6`，返回
`model:1,width:52,height:52,driver:fm6124,frameFade:1,streamRecv:1`。
二进制接收失步后先尝试发送错误通知，约 250 ms 后主动断连，App 必须重新连接。
`rotary_cache_clear` 可无条件清理当前连接缓存；正常退出图纸优先使用带 session 的
`rotary_frame_clear`，避免旧页面清理新图纸。断开/重连也会清理缓存。

本地色号/区域画面来自 App 预载，不是固件内置菜单；未预载时继续发送原有旋钮事件。
3.2.6 曾沿用全白逻辑；3.2.7 已恢复下述长按菜单。


## 3.2.7 原版长按色号菜单恢复

恢复三张色号卡片、沿旋钮附近的轨迹滑动、完成卡片退出和烟花动画。

- `rotary_frame_begin` 接收现有 App 的 `labels` 与 `colors` 数组，与 `order` 一一对应。
  `colors` 单项为 RGB 三元组；旧客户端省略元数据时显示 region ID 和中性灰。
- 长按进入菜单，松开保持菜单；旋钮逐项切换；单击显示选中色号区域；再次长按退出到选中区域。
- 双击保留完成通知 `dbl_click` 及 `rotary_session` / `rotary_region`，由 App 更新完成状态和图纸顺序。
  全部完成时 App 的 `rotary_frame_complete` 启动烟花，旧 session 不能完成新图纸。
- 长按不再发 `all_white_on/off`，使用 `menu_open/close` 事件；松开只发事件，不关闭菜单。
- 没有完整预载图纸时长按显示 `OPEN APP / LOAD ART`，不会显示全白或编造图纸颜色。
- `capabilities` 增加 `rotary_menu_v1:true`；F6 增加 `rotaryMenu:1`；`rotary_stats` 增加 `menu_open`。

JSON 行上限增至 4096 字节（含 NUL），支持 64 色元数据。运行时命令队列降至 4 项
以限制内部 RAM；8 KiB 菜单画布放在 PSRAM。串行正文、READY/ACK 和连接会话规则不变。
