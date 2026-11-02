# PixelPad BLE 通信协议文档

## 3.3.5 当前图纸驻留

`capabilities` 返回 `rotary_frames_v1:true,max_regions:64`。

- `rotary_frame_begin`：`session`、`order`（非负 region ID 数组，1～64 个唯一 ID）、`wrap`。
- `rotary_frame_store`：`session`、`region`、`size`。`region:-1` 是完整底图 8112 字节；其他 ID 是 16224 字节的色号图 + 区域图。必须等 `status:ready` 再发送正文，等 `status:ok` 才算缓存成功。
- `rotary_frame_select`：`session`、`region`、`code`、`hold_ms`（0～2000）、`fade_ms`（0～500）、`after_input`。全量缓存就绪且序号同步后启用本地旋钮；`region:-1` 显示底图并保留未选择的初始规则。
- `rotary_frame_clear`：仅清理匹配 `session` 的缓存。
- `rotary_stats`：查询 session、loaded、count、armed、region、local_steps。

本地旋钮通知仍是原有 `evt`，附加非零 `rotary_session` 与 `rotary_region`。匹配当前图纸时，App 采用通知中的 region 更新游标并停止回传画面；不同图纸的旧通知不能作为新图纸的步进。


## 3.3.4 旋钮队列和连续接收

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


> RTOS 会话、队列、ACK、统计及 OTA 互斥约束见 [RTOS 升级说明](docs/RTOS_UPGRADE.md#协议兼容)。
> 3.3.1：图片接收允许持续进展的慢传，5 秒无新数据或总计 30 秒才超时。二进制失步后先发错误通知，再由设备主动断连，需重新连接。

> 3.3.3：FM6373C 能力特征 `7DA35D3C-AD91-41BA-8A6B-D375CC1116F6` 提供 `frameFade:1`。`recv` 可带 `fade_ms:200`（0～500 毫秒），在完整接收目标帧后由灯板本地过渡；不增加二进制字节数。ACK 表示目标已接收并提交，最终显示状态可读 `display_stats` 的 `fading`、`shown` 和 `shown_hash`。省略字段时即时显示。
> `display_stats` 新增 `committed`、`shown` 和 `shown_hash`（活动 DMA 缓冲区对应 RGB 快照的 FNV-1a 32 位哈希，十进制），用于确认内容交接，不能替代光学测量。

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
