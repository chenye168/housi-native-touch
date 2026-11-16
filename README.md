# 原声触控实现 · Native Touch for an External USB Touchscreen on macOS

> 让一台普通外接 USB 触控屏（HOUSI 14" USB-C）在 macOS 27 上被 WindowServer **当作原生触控屏接纳**，
> 从而实现 Safari 滚动/捏合、菜单栏点击等原生触控交互——**不关 SIP、不写驱动、不需要 Apple 私有授权，
> 且开机全自动、重启后无需拔插或手动执行任何命令**。

[![platform](https://img.shields.io/badge/platform-macOS%2027.2.1%20(26B131)-black)]()
[![arch](https://img.shields.io/badge/arch-arm64-blue)]()
[![sip](https://img.shields.io/badge/SIP-on-green)]()
[![license](https://img.shields.io/badge/license-MIT-yellow)](LICENSE)

---

## 1. 这个项目做了什么

macOS 默认把外接 USB 触控屏当作「鼠标模拟器」：能拖动指针，但没有原生触控语义
（没有滚动惯性、没有双指捏合、没有直接触控事件）。系统日志会明确拒绝：

```
[com.apple.BackBoard:TouchEvents] ignoring digitizer event for display <main>
  from unsupported service: (<BKIOHIDService: … name: USB2IIC_CTP_CONTROL …>)
```

本项目做了三件事：

| # | 工作 | 产物 |
|---|---|---|
| 1 | **逆向定位** WindowServer / BackBoard 的接纳条件（反汇编级） | [analysis/](analysis/README.md) |
| 2 | **实现** 属性注入 + 开机自动 USB 重枚举 + 自动恢复多点模式 | [tools/](tools/README.md) |
| 3 | **验证** 真实重启后的端到端触控，并逐项核对 G01–G28 手势清单 | [verification/](verification/TOUCH-GESTURE-MATRIX.md) |

结论：**已完成**。真实重启验证中，Safari 单指滚动 / 双指捏合、菜单栏点击均正常，
`unsupported service` 日志计数为 0。细节见 [REPORT.md](REPORT.md) 与 [STATUS.md](STATUS.md)。

---

## 2. 根因（一句话版）

WindowServer 只在 digitizer **接入（attach）的那一刻**读取该 HID 事件服务的
`displayUUID` 属性（且必须是 **CFString** 形式的显示器 UUID，并命中 UUID→WSDisplay 映射）。
读不到就丢弃该 digitizer 的全部触摸事件。因此最小必要条件只有一个：

> **在 WindowServer attach 之前**，让该 digitizer 的 HID 事件服务带上
> `displayUUID = CFString("<该触控屏的显示器 UUID>")`。

完整反汇编证据（`_queue_displayUUIDForDigitizerService:`、`handle_digitizer_added` 等）见
[analysis/acceptance-check.md](analysis/acceptance-check.md)。

---

## 3. 解决方案（三层，全自动）

```
开机
 └─ local.housitouchd（root LaunchDaemon）
      ├─ 扫描 digitizer 服务并注入 displayUUID
      ├─ 若设备已被 WindowServer 无属性 attach → 触发一次 USB 重枚举（约 70ms）
      │   └─ IOKit 服务出现通知 → 抢在 WindowServer attach 前注入
      └─ 以控制台用户身份启动 HOUSITouchHelper.app
           └─ 写 feature report 0x13 → Input Mode = 2（多点模式）
```

对应代码：`tools/daemon/housitouchd.c`、`tools/helper/housitouch-setmode.c`。
安装位置与卸载方式见 [REPORT.md §4](REPORT.md)。

---

## 4. 快速开始

```bash
# 0) 只编译，不安装（推荐先做）
make build

# 1) 安装（需管理员密码）
sudo make install

# 2) 一次性授权：系统设置 → 隐私与安全性 → 输入监控 → 打开 HOUSITouchHelper

# 3) 完成
```

> ⚠️ **HOUSITouchHelper.app 是 ad-hoc 签名**。重新编译/重签会让「输入监控」授权失效，
> 需要重新授权。日常使用请勿反复重建。

**完全回退**：

```bash
sudo make uninstall
```

回退后无需重启，触控退回系统默认的鼠标模拟模式。

**日常体检**：

```bash
make check
```

---

## 5. 仓库结构

```
.
├── README.md                 ← 你在这里
├── REPORT.md                 ← 最终实现与验证报告（主文档）
├── GESTURES.md               ← G01–G28 手势清单分析结论
├── STATUS.md                 ← 一页纸当前状态 + 卸载命令
├── PRIVACY.md                ← 隐私处理说明（入库前做了什么脱敏）
├── CHANGELOG.md              ← 变更记录
├── CONTRIBUTING.md           ← 贡献指南
├── SECURITY.md               ← 安全边界与负责任披露
├── LICENSE                   ← MIT
├── NOTICE.md                 ← 第三方内容归属与免责声明
├── Makefile                  ← 构建 / 安装 / 卸载入口
├── docs/ARCHITECTURE.md      ← 设计说明：为什么是这三层
├── analysis/                 ← WindowServer/BackBoard 接纳判定反汇编与复现步骤
│   ├── README.md             ← 怎么复现
│   ├── acceptance-check.md   ← 结论（反汇编级）
│   ├── disasm/               ← 关键函数反汇编与命令记录
│   └── scripts/              ← annotate.py / dscmap.py / xrefs.py
├── research/                 ← 被否决的路线与门槛（HID 插件 / DriverKit / 虚拟设备 / Sidecar）
│   └── hid-channels/
├── tools/                    ← 所有可执行工具（源码 + 构建脚本）
│   ├── daemon/               ← 开机注入 daemon + USB 重枚举
│   ├── helper/               ← 多点模式辅助 App
│   ├── harness/              ← AppKit / IOHIDManager 探针 + 运行样例
│   ├── gesturewatch/         ← 手势事件观察器
│   └── *.c / *.m / *.swift   ← 独立诊断探针
├── evidence/                 ← 精选实测证据（大体积原始日志未入库，见其 README）
└── verification/             ← 手势验证清单 + 可视化实验台
```

---

## 6. 已知限制

| 限制 | 说明 |
|---|---|
| Dock 点击 | WindowServer 已把触摸投递到 Dock 连接，但 Dock 的 direct-touch 处理对外接 digitizer 不响应。builtIn 分类 / 主屏切换 / 私有开关均已排除，属 macOS 内部限制。见 [REPORT.md §7](REPORT.md)。 |
| 触控板系统手势 | 三/四指、边缘手势等 10 项走 `WSIndirectGestureProcessing`（indirect 管线），不经过外接 digitizer 的直接触控管线。见 [GESTURES.md](GESTURES.md)。 |
| 窗口控制“触控放大” | 依赖 hover/direct-touch 适配；本机 AppKit 探针收不到 `NSEventTypeDirectTouch`，系统只把触摸合成为普通点击。 |
| 系统升级 | 依赖私有属性 `displayUUID` 与 HID 服务重建时序，macOS 升级后可能失效。 |

---

## 7. 安全与隐私

- **不修改任何系统文件、不关闭 SIP、不使用 Apple 私有 entitlement**。
  全部改动仅限：`/Library/LaunchDaemons/local.housitouchd.plist`、`/usr/local/libexec/housitouchd*`、
  `/Library/LaunchAgents/local.housitouch.setmode.plist`、`/Applications/HOUSITouchHelper.app`、
  `/var/db/housitouchd.lastreset`、`/var/log/housitouchd.log`、`/tmp/housitouch-setmode.log`。
- 需要一次性「输入监控」授权（写 HID feature report 必需）。
- **本仓库不含任何个人隐私数据**：家目录路径、用户名、主机名、机器序列号在入库前已脱敏，
  见 [PRIVACY.md](PRIVACY.md)。仓库同时**刻意排除**了 6.1 GB 的 Apple 系统库提取缓存、
  编译产物与超大原始日志。
- 安全模型与披露方式见 [SECURITY.md](SECURITY.md)。

---

## 8. 适用性与复现前提

本项目的核心结论是**通用机制**（`displayUUID` 注入 + attach 时序），但具体常量是绑硬件的：

| 需要按你的设备替换 | 位置 |
|---|---|
| 显示器 UUID | `tools/daemon/housitouchd.c` 中 `kTargetUUID` |
| VID / PID / UsagePage / Usage | 见 [analysis/README.md](analysis/README.md) 与 `tools/harness/README.md` |

换设备后请先跑 `tools/harness/build/hidprobe --list` 与
`ioreg -r -c AppleUserHIDEventService -l -w0 | grep -a displayUUID` 确认。

---

## 9. 许可

[MIT](LICENSE)。Apple 系统二进制/提取物、第三方调研摘录的权利归属见
[NOTICE.md](NOTICE.md)——反汇编片段仅用于互操作性与安全研究，Apple 二进制本身不在本仓库内分发。

---

## English summary

This repository documents and implements **native touchscreen support for an external USB
digitizer on macOS 27**, without disabling SIP, without a kernel driver, and without Apple
entitlements. WindowServer only reads the `displayUUID` property on the digitizer's HID event
service **at attach time**; if it is missing, every touch event is dropped as coming from an
`unsupported service`. The project injects that property from user space, forces a USB
re-enumeration when boot ordering loses the race, and restores multi-touch input mode
automatically — so the touchscreen works natively (Safari scroll/pinch, menu bar taps) after a
cold boot with no manual steps. Evidence, disassembly-level root cause analysis, and the
rejected alternative approaches (HID plugins, DriverKit, virtual devices, Sidecar) are all
included. See [REPORT.md](REPORT.md) for the full write-up.
