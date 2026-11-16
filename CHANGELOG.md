# 变更记录

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

---

## [0.1.0] — 2026-11-16

**首个公开版本：外接 USB 触控屏在 macOS 27 上的原生触控从「不可用」到「开机全自动可用」。**

本版本把一次完整的调查研究沉淀为可复现的仓库，包含四段工作：

### 研究（上午）
- 定位系统日志 `ignoring digitizer event for display <main> from unsupported service`。
- 逐一验证四条被否决的路线并记录门槛：HID 插件注入、DriverKit 系统扩展、
  虚拟 HID 设备（`IOHIDUserDevice`）、Sidecar/BKDirectTouch 旁路。
  → `research/hid-channels/FEASIBILITY.md`（含 entitlements、签名、SIP 要求与实测输出）。
- 建立反汇编环境：用 `/usr/lib/dsc_extractor.bundle` 提取 dyld 共享缓存，
  以 `tools/dscx.c` 驱动，`analysis/scripts/` 提供地址映射、交叉引用、注释脚本。

### 根因（反汇编确认）
- 证明 WindowServer 只在 digitizer **attach 时**读取 `displayUUID`，且必须是 CFString
  并命中 UUID→WSDisplay 映射；缺失即丢弃事件。
- 排除私有 feature flag `backboardd/SupportExternalTouchscreen`（SkyLight 侧无 fallback）。
  → `analysis/acceptance-check.md`、`analysis/disasm/`

### 实现（下午）
- `housitouchd`：开机注入 `displayUUID`；用 IOKit 服务出现通知抢在 attach 之前；
  竞争失败时以 `IOUSBHostDevice resetWithError:` 强制重枚举（同 boot 只做一次）。
- `HOUSITouchHelper.app`：写 feature report `{0x13,0x02,0x00}` 把 Input Mode 设为 2（多点）。
- 安装/卸载脚本、LaunchDaemon/LaunchAgent、ad-hoc 签名与 TCC 授权流程。
  → `tools/daemon/`、`tools/helper/`

### 验证
- 真实重启（14:07）验证：`digitizer attached … displayUUID: 30D985CD-…`，
  `unsupported service` 计数 **0**，用户实测 Safari 滚动/捏合、菜单栏点击正常。
- 逐项核对 G01–G28 手势清单：25 ✅ / 1 ⚠️ / 12 ❌，并把 12 项失败归因为
  「2 项外接触控限制 + 10 项触控板 indirect 管线」。
  → `verification/TOUCH-GESTURE-MATRIX.md`、`GESTURES.md`
- Dock 点击专项调查（§7）：证明触摸已投递到 Dock 连接但 Dock 不响应，
  排除 builtIn 分类/主屏切换/私有开关三种假设。

### 仓库
- 新增构建入口 `Makefile`、`tools/*/build.sh`、`tools/build-standalone.sh`。
- 新增 `README.md`、`docs/ARCHITECTURE.md`、`CONTRIBUTING.md`、`SECURITY.md`、
  `PRIVACY.md`、`CHANGELOG.md`、`LICENSE`（MIT）。
- 隐私脱敏：家目录路径、用户名、主机名、机器序列号全部清理（见 `PRIVACY.md`）。
- 排除 6.1 GB 系统库缓存、编译产物与超大原始日志，仓库保持轻量可读。
