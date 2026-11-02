# PixelPad RTOS 开发与分支规范

本文是固件开发、代码审查和工作交接的约定。代码行为、构建参数和协议约束仍以源码、`platformio.ini` 及 `BLE_PROTOCOL.md` 为准。

## 分支模型

- `main`：唯一稳定集成和发布主线，禁止直接提交。
- `feature/<scope>-<description>`：新增功能，例如 `feature/ble-image-flow-control`。
- `fix/<scope>-<description>`：缺陷修复，例如 `fix/input-rotary-drops`。
- `refactor/<scope>-<description>`：结构调整，例如 `refactor/protocol-session`。
- `test/<scope>-<description>`：测试和验证，例如 `test/display-frame-boundary`。
- `docs/<scope>-<description>`：文档变更，例如 `docs/handoff-guide`。
- `chore/<scope>-<description>`：依赖、构建和工具维护，例如 `chore/platformio-deps`。

`firmware/<board>` 和 `app/<scope>` 只用于需要长期维护的产品线。当前产品线见 README 的分支表；不要为一次性功能创建长期分支，也不要新增 `develop`、`release` 等常驻分支。

分支名统一使用小写、短横线和 `/`，避免空格、中文、个人姓名和无意义编号。短期分支完成合并后删除。涉及主线历史迁移时，先建立带日期的备份引用，再使用受保护的 PR 或经过说明的强制更新。

## 开发与审查要求

1. 从 `main` 创建短期分支；硬件专用修改从对应 `firmware/<board>` 分支开始，并在 PR 中说明原因。
2. 修改 BLE 命令、ACK、数据边界、旋钮事件或 OTA 时，同时更新 `BLE_PROTOCOL.md`，并注明 App/后端影响。
3. 修改显示时序、引脚、颜色位深、分区或 PlatformIO 环境时，注明目标板和不兼容配置。
4. PR 至少附上受影响环境的 `pio run -e <env>` 和 `python tools/test_host.py` 结果；显示、输入、BLE 或 OTA 变更应附实板日志，不能验证时明确写出原因。
5. 不提交 `.pio/`、临时串口日志、设备密钥或本地构建产物。协议或版本变化必须在 README/验证文档留下可追溯记录。

## 发布与交接清单

- [ ] 版本宏、目标硬件和分支名称一致。
- [ ] 默认构建及受影响构建通过；主机回归测试通过。
- [ ] 实板验证记录包含设备、固件 SHA-256、测试范围和未完成项。
- [ ] BLE/OTA/后端/App 变更已通知对应负责人。
- [ ] PR 描述包含变更摘要、风险、回滚提交或 tag，以及产物位置。
- [ ] 发布使用版本 tag（如 `v3.2.6`），不直接在 `main` 上改写历史。

新成员按以下顺序阅读：`README.md` → `platformio.ini` → `BLE_PROTOCOL.md` → `docs/RTOS_REVIEW.md` → `docs/COLOR_RESTORE.md`/`docs/VALIDATION.md` → `src/main.cpp` 和相关模块。
