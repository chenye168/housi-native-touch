# 架构说明：为什么是「注入 + 重枚举 + 恢复模式」三层

本文解释设计取舍。现象与结论见 [REPORT.md](../REPORT.md)，反汇编证据见
[analysis/acceptance-check.md](../analysis/acceptance-check.md)。

---

## 1. 系统侧的约束条件

WindowServer 的 direct-touch 管线对 digitizer 有一条**一次性**前置检查：

```
digitizer attach
   └─ _queue_displayUUIDForDigitizerService:
        ├─ 读 HID 事件服务的 "displayUUID" 属性
        ├─ 必须是 CFString（CFNumber / CFData 均被拒）
        └─ 必须命中 UUID → WSDisplay 映射
             ├─ 命中  → 建立 BKDirectTouchState，事件正常投递
             └─ 未命中 → "unsupported service"，该 digitizer 事件全部丢弃
```

关键点：**检查只发生在 attach 时**。attach 之后再怎么补属性都不生效——
这正是「先装 daemon 再插屏也不行」的原因：屏在开机时就已经 attach 过了。

由此推出三个必须同时解决的问题：

| 问题 | 后果 | 本项目对策 |
|---|---|---|
| P1 属性缺失 | 事件被丢弃 | 用户态注入 `displayUUID`（IOKit `IORegistryEntrySetCFProperty`） |
| P2 时序竞争 | daemon 晚于 attach | IOKit 服务出现通知，抢在 WindowServer attach 之前 |
| P3 开机缓存 | 设备已被无属性 attach | 强制 USB 重枚举，让 HID 服务重建，再抢一次 |

---

## 2. 三层设计

### 2.1 注入层（`housitouchd`）

- 匹配 `IOHIDEventService` 中 `PrimaryUsagePage=13, PrimaryUsage=4` 的 HOUSI digitizer。
- 用 `IOServiceAddMatchingNotification(kIOFirstMatchNotification)` 注册回调，
  服务一发布就注入——此时 WindowServer 还没走到 attach。
- 注入等价于：
  ```bash
  hidutil property --matching '{"PrimaryUsagePage":13,"PrimaryUsage":4}' \
      --set '{"displayUUID":"<uuid>"}'
  ```

### 2.2 重枚举层（同进程内的兜底）

- 若启动时设备**已经存在**（说明可能已被无属性 attach），执行一次
  `IOUSBHostDevice resetWithError:`（实测约 70 ms）。
- 重置后服务重建 → 走 2.1 的通知路径重新注入。
- 用 `/var/db/housitouchd.lastreset` 记录 boottime，**同一 boot 只重置一次**，
  防止「重置→重建→再重置」的循环。

### 2.3 模式恢复层（`HOUSITouchHelper`）

- HOUSI 面板掉电/重枚举后会回到单点模式，必须写 feature report：
  `IOHIDDeviceSetReport(kIOHIDReportTypeFeature, 0x13, {0x13,0x02,0x00}, 3)`。
- 必须带 ReportID 且长度为 3——这是实测出来的硬性要求，其它写法被设备忽略。
- 由 root daemon 在重枚举之后以控制台用户身份拉起
  （`launchctl asuser <uid> open -g -j -a …`），同时用
  `/Library/LaunchAgents/local.housitouch.setmode.plist` 做登录兜底。
- 设置 Input Mode 不会导致 HID 服务重建，因此不会破坏已经建立的 displayUUID 关联。

---

## 3. 为什么不用别的方案

| 方案 | 阻塞点 | 记录 |
|---|---|---|
| HID 插件（`/Library/HIDPlugins`） | 需要 Apple 签名/entitlement，第三方插件不被加载 | [research/hid-channels/FEASIBILITY.md](../research/hid-channels/FEASIBILITY.md) |
| DriverKit 系统扩展 | 需要 entitlement + 用户批准 + 无法直接改 WindowServer 行为 | 同上 |
| 虚拟 HID 设备（`IOHIDUserDevice`） | 能造出 digitizer，但仍要 `displayUUID` 才能被接纳 | 同上 |
| Sidecar / BKDirectTouch 旁路 | 依赖 Continuity 私有通道 | 同上 |
| 私有 feature flag `SupportExternalTouchscreen` | 只在 BackBoard 侧有 fallback，SkyLight 的 `handle_digitizer_added` 没有 | [analysis/](../analysis/acceptance-check.md) |
| 关闭 SIP 改二进制 | 违背「不降低系统安全性」的前提 | 设计上排除 |

结论：**用户态属性注入是唯一同时满足「不关 SIP、不改系统文件、不需要 Apple 授权」的路径**。

---

## 4. 运行时数据流

```
USB digitizer (VID 0x32D7 / PID 0x0001, UsagePage 0x0D / Usage 0x04)
        │  HID 事件服务（IOHIDEventService）出现
        ▼
housitouchd ──(1) 注入 displayUUID（CFString）──► 服务属性表
        │
        │  WindowServer attach，读属性 → 命中 → 建立 BKDirectTouchState
        ▼
WindowServer direct-touch 管线 ──► 前台 App（Safari 滚动/捏合）、菜单栏
        │
        └─(2) 若竞争失败：USB 重枚举 → 服务重建 → 回到 (1)
                    │
                    └─(3) 重枚举后：以控制台用户身份启动 HOUSITouchHelper
                              └─ 写 feature report 0x13 → Input Mode = 2
```

---

## 5. 失败模式与观测点

| 失败模式 | 观测方法 | 期望值 |
|---|---|---|
| 注入没发生 | `ioreg … \| grep displayUUID` | 应出现该 UUID |
| 注入晚于 attach | `log show … \| grep 'digitizer attached'` | attach 行里应带 `displayUUID:` |
| 被判定 unsupported | `log show … \| grep 'unsupported service'` | 计数应为 0 |
| 单点模式 | `hidprobe --setmode --dry-run` | `mode=2` |
| daemon 没起来 | `launchctl print system/local.housitouchd` | 有 PID |

`make check` 会把上述观测点一次性打印出来。

---

## 6. 硬件常量与可移植性

代码里有几个**绑硬件**的常量，换设备必须改：

| 常量 | 现值 | 位置 |
|---|---|---|
| 显示器 UUID | `30D985CD-BE3F-4C0E-B055-C5E22CFF2E07` | `tools/daemon/housitouchd.c: kTargetUUID` |
| 匹配条件 | `PrimaryUsagePage=13, PrimaryUsage=4` | 同上 `kMatching` |
| Feature Report | `0x13`，payload `{0x13,0x02,0x00}` | `tools/helper/housitouch-setmode.c` |

后续可以把它们提取为 plist/命令行参数，但当前刻意保持硬编码——
一次验证过的常量比多一层配置更不容易出错。
