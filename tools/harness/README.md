# tools/harness — AppKit 原生触控探针 + HID 多触点探针

本目录是在 **macOS 27.2.1 (26B131) / Apple M1 Pro / 仅 Command Line Tools（无 Xcode、无 DriverKit SDK）**
环境下实测通过的触控验证工具集，用于回答两个问题：

1. **AppKit 层**：外接 HOUSI 触控屏（CGDirectDisplayID=3，
   UUID=`30D985CD-BE3F-4C0E-B055-C5E22CFF2E07`，1600×1066，rotation 180）的触摸
   会不会以 `NSEventTypeDirectTouch (37)` / gesture 事件到达普通 App？
2. **HID 层**：HOUSI 的 USB2IIC_CTP_CONTROL（VID `0x32D7` / PID `0x0001`）Digitizer 接口
   的 Report Descriptor 到底长什么样、input report 怎么编码、如何通过 Feature Report 把
   **Input Mode (0x0D/0x52)** 设为多点模式 2，以及系统当前实际模式是什么。

工具全部只读 + 可控写入（写入仅限触控控制器的 Input Mode feature report），
不修改系统文件、不使用 sudo、不关闭 SIP。

---

## 0. 文件清单

| 文件 | 说明 |
|---|---|
| `touchprobe.swift` | AppKit 程序源码：打印屏幕信息、监听 DirectTouch/Gesture、写日志 |
| `hidprobe.c` | IOHIDManager 探针源码：枚举/描述符解析/input report dump/SetReport+回读 |
| `build.sh` | 一键构建两个二进制到 `build/` |
| `build/touchprobe` | 构建产物（`swiftc -O`） |
| `build/hidprobe` | 构建产物（`clang -O2 -Wall`） |
| `samples/` | 本机实际运行输出样例（见文末索引） |
| `README.md` | 本文档 |

`build/` 与 `samples/` 均可删除后重新生成。

---

## 1. 环境与构建

已验证环境：

```
macOS 27.2.1 (26B131), Apple M1 Pro (MacBookPro18,1), SIP 开启
/Library/Developer/CommandLineTools（swift-driver 1.168.6 / Swift 6.4.2 / clang 21.0.0）
/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk（也有 MacOSX27.2.sdk）
无 /Library/Developer/CommandLineTools/SDKs/DriverKit*.sdk，无 Xcode.app
```

构建（在 WORK 根目录下）：

```bash
cd tools/harness
./build.sh
```

实际构建输出（2026-11-16 本机）：

```
== 工具链 ==
swiftc: swift-driver version: 1.168.6 Apple Swift version 6.4.2 (swiftlang-6.4.2.1.9 clang-2100.4.1.1)
clang : Apple clang version 21.0.0 (clang-2100.4.1.1)
SDK   : /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk

== [1/2] 编译 touchprobe (Swift + AppKit) ==
== [2/2] 编译 hidprobe (C + IOKit) ==

== 构建完成 ==
-rwxr-xr-x@ 1 chen  staff   73672 Nov 16 11:38 build/hidprobe
-rwxr-xr-x@ 1 chen  staff  209640 Nov 16 11:38 build/touchprobe
```

> 不需要 Xcode、不需要 Info.plist、不需要签名即可编译运行（TCC 授权见第 4 节）。

---

## 2. touchprobe（AppKit 原生触控验证）

### 2.1 用法

```bash
./build/touchprobe [--duration 秒] [--logfile 路径] [--no-overlay] [--all-events]
```

| 参数 | 说明 |
|---|---|
| `--duration N` | N 秒后自动退出（默认 0 = 一直运行，Ctrl-C / SIGTERM 退出） |
| `--logfile PATH` | 事件日志追加写（默认 `WORK/evidence/touchprobe.log`，目录不存在会自动创建） |
| `--no-overlay` | 不在 HOUSI 屏上创建全屏触控接收窗口（默认创建） |
| `--all-events` | 除 DirectTouch/Gesture 外监听全部 AppKit 事件（调试用，事件很多） |
| `--help` | 帮助 |

默认日志路径由二进制位置推导：`tools/harness/build/touchprobe` → `WORK/evidence/touchprobe.log`；
若二进制不在该结构下则回退到 `$PWD/evidence/touchprobe.log`。

### 2.2 行为

启动后：

1. 打印所有 `NSScreen` 的 `localizedName / CGDirectDisplayID / CGDisplayCreateUUIDFromDisplayID /
   touchCapabilities / frame / visibleFrame / backingScaleFactor`，并与 `CGGetOnlineDisplayList`
   的 `CGDisplayBounds / rotation` 对照；
2. 打印 HOUSI 屏（UUID 匹配）的坐标映射参考：`NSTouch.normalizedPosition (0~1, 面板左下为
   (0,0))` → NSScreen 全局坐标的直映射，以及 rotation 180° 翻转候选；
3. 创建两个窗口：
   - **主窗口**（浮于最前，level=`floating`），显示日志文本与提示“请触摸 HOUSI 屏幕”；
   - **HOUSI 覆盖窗口**（无边框、覆盖 HOUSI 全屏、level=`floating`，`TouchView` 接受
     direct touch），内容显示“请触摸此屏幕（HOUSI 触控屏）”。可用 `--no-overlay` 关闭；
4. 同时注册 **local monitor**（`NSEventMaskDirectTouch | NSEventMaskGesture`）与
   **global monitor**（需要辅助功能/输入监控权限才可能收到其他 App 事件），并在
   `TouchView` 的 `touchesBegan/Moved/Ended/Cancelled` 中二次捕获 direct touch；
5. 每个事件写一行带 ISO8601 毫秒时间戳的日志：**stdout + `--logfile` 文件 + 主窗口文本视图**；
6. 退出时输出 `=== SUMMARY ===`（各类事件计数；若 0 事件会给出 WindowServer 丢弃事件的提示）。

日志行格式（节选）：

```
2026-11-16T11:33:32.949+08:00 [SYS  ] === SUMMARY ===
2026-11-16T11:33:32.949+08:00 [EVT  ] src=local type=directTouch(37)(37) subtype=0 eventNumber=123 timestamp=... phase=none(0) locationInWindow=(... ) window=1330 ... touches=2
2026-11-16T11:33:32.949+08:00 [TOUCH]   touch[0] idHash=... phase=began type=direct normalized=(0.50123,0.50045) resting=no deviceSize=(1600.0,1066.0) locRoot=(...) ->HOUSI直接(...) rot180(...) device=<IOHIDDevice ...>
```

`[EVT ]` 行包含：`eventNumber / type / subtype / timestamp / phase / locationInWindow /
window(windowNumber) / windowFrame / screen / screenPoint / modifierFlags / touches 数`；
`[TOUCH]` 行包含每个 `NSTouch`：`identity hash / phase / type(direct|indirect) /
normalizedPosition / resting / deviceSize / location(in:nil) / 映射候选 / device 描述`。

### 2.3 权限

- 本程序的 **local monitor 与视图 touches 不需要额外权限**；
- **global monitor** 需要「辅助功能」或「输入监控」授权才可能收到其它 App 的事件；
  未授权时只是收不到，不会崩溃；
- 若 WindowServer 在 digitizer 服务层丢弃事件（`ignoring digitizer event for display <main>
  from unsupported service`），AppKit 层不会有任何事件 —— 这正是本探针要验证的现象。

### 2.4 本机实测输出（`samples/touchprobe-duration2.txt` 节选）

```
2026-11-16T11:33:30.934+08:00 [SYS  ] === NSScreen.screens (2) ===
2026-11-16T11:33:30.934+08:00 [SYS  ] [0] name=Built-in Retina Display displayID=1 uuid=37D8832A-2D66-02CA-B9F7-8F30A301B230 touchCapabilities=1 (MultiTouch) main=YES
2026-11-16T11:33:30.935+08:00 [SYS  ]     NSScreen.frame={{0, 0}, {1312, 848}} visibleFrame={{0, 0}, {1312, 822}} backingScale=2.0
2026-11-16T11:33:30.935+08:00 [SYS  ]     CGDisplayBounds={{0, 0}, {1312, 848}} rotation=0°
2026-11-16T11:33:30.935+08:00 [SYS  ] [1] name=<空> displayID=3 uuid=30D985CD-BE3F-4C0E-B055-C5E22CFF2E07 touchCapabilities=1 (MultiTouch) main=no
2026-11-16T11:33:30.936+08:00 [SYS  ]     NSScreen.frame={{-134, -1066}, {1600, 1066}} visibleFrame={{-134, -1066}, {1600, 1036}} backingScale=1.0
2026-11-16T11:33:30.936+08:00 [SYS  ]     CGDisplayBounds={{-134, 848}, {1600, 1066}} rotation=180°
2026-11-16T11:33:30.936+08:00 [SYS  ]     HOUSI? = YES (UUID 匹配)
...
2026-11-16T11:33:30.941+08:00 [SYS  ] NSEventTypeDirectTouch = 37, NSEventTypeGesture = 29
2026-11-16T11:33:30.942+08:00 [SYS  ] NSEventMaskDirectTouch raw = 137438953472
...
2026-11-16T11:33:30.944+08:00 [SYS  ] 启动时 NSApp.windows=2
2026-11-16T11:33:30.944+08:00 [SYS  ]   win[0] number=1342 title=touchprobe — AppKit 原生触控验证（NSEventTypeDirectTouch=37） visible=YES level=3 frame={{198, 22}, {1000, 792}} screen=Built-in Retina Display
2026-11-16T11:33:30.945+08:00 [SYS  ]   win[1] number=1343 title= visible=YES level=3 frame={{-134, -1066}, {1600, 1066}} screen=<空>
...
2026-11-16T11:33:32.949+08:00 [SYS  ] duration=2.12s directTouchEvents=0 gestureEvents=0 otherEvents=0
```

注意：`NSScreen.screens` 中两块屏的 `touchCapabilities` 都是 `1 (MultiTouch)`（与 lead 实测一致），
但 2 秒自动运行期间没有触摸，因此事件计数为 0。**归因需要人手实际触摸**（见第 5 节）。

---

## 3. hidprobe（HID Digitizer 探针）

### 3.1 用法

```bash
./build/hidprobe --list [--descriptor] [--elements]     # 枚举 + 描述符解析（默认动作）
./build/hidprobe --dump [--seconds N] [--interface digitizer|mouse|all]
./build/hidprobe --setmode [--mode N] [--dry-run] [--force] [--restore]
./build/hidprobe --request-access
./build/hidprobe [--vid 0x32d7] [--pid 0x0001] [--logfile PATH]
```

主要参数：

| 参数 | 说明 |
|---|---|
| `--list` | 枚举 VID/PID 匹配的所有 HID interface（默认动作） |
| `--descriptor` | 额外输出完整 Report Descriptor hex |
| `--elements` | 额外输出所有 Feature Report 元素（`IOHIDDeviceCopyMatchingElements`） |
| `--dump` | 打开 Digitizer，打印 input report（reportID + 原始 hex + 触点解码） |
| `--seconds N` | `--dump` 时长，默认 10，0 = Ctrl-C 退出 |
| `--setmode` | 把 Input Mode (0x0D/0x52) 设为 `--mode`（默认 2 = multi-touch），并回读验证 |
| `--dry-run` | 只打印将发送的 Feature Report，不写设备 |
| `--force` | Input Mode 已等于目标值时仍发送一次 SetReport |
| `--restore` | 写入并验证后恢复原 Input Mode |
| `--request-access` | 调 `IOHIDRequestAccess(kIOHIDRequestTypeListenEvent)` 主动触发授权提示 |
| `--logfile PATH` | 追加写日志 |

内部子命令（`--setmode` 自动调用，也可手动运行，输出无时间戳）：
`--inspect`、`--open-close`、`--read-mode`、`--get-report [RID]`、`--write-mode N`。

### 3.2 `--list` 实测（`samples/hidprobe-list.txt` 节选）

```
[0] IOHIDDevice transport=USB product=USB2IIC_CTP_CONTROL manufacturer=wch.cn
    VID=0x32D7 PID=0x0001 PrimaryUsagePage=0x01 PrimaryUsage=0x02  <-- Mouse (0x01/0x02)
    ReportDescriptor=75 bytes MaxInputReportSize=7 ...
[1] IOHIDDevice transport=USB product=USB2IIC_CTP_CONTROL manufacturer=wch.cn
    VID=0x32D7 PID=0x0001 PrimaryUsagePage=0x0D PrimaryUsage=0x04  <-- Digitizer (0x0D/0x04)
    Version=262 LocationID=0x02100000 Serial=- RequiresTCCAuthorization=YES
    ReportDescriptor=847 bytes MaxInputReportSize=64 MaxOutputReportSize=12 MaxFeatureReportSize=257
  [Input reports]
    Input ReportID=0x54: data=63 bytes, on-wire=64 bytes (含 ReportID)
    Input ReportID=0x60: data=8 bytes, on-wire=9 bytes (含 ReportID)
  [Output reports]
    Output ReportID=0x60: data=1 bytes ...
    Output ReportID=0x63: data=11 bytes ...
  [Feature reports]
    Feature ReportID=0x11: data=1 bytes  field 000D/0055 logical=0..10
    Feature ReportID=0x12: data=1 bytes  field 000D/0060 logical=0..1 + 7 bit padding
    Feature ReportID=0x13: data=2 bytes (on-wire=3)
      bit=0 size=8 idOffset=0 usagePage/Usage=000D/0052 logical=0..10   <-- Input Mode (多点=2)
      bit=8 size=8 idOffset=1 usagePage/Usage=000D/0053 logical=0..10
    Feature ReportID=0x44: data=256 bytes（vendor FF00/FFC5，大块数据不读取）
    Feature ReportID=0x64: data=63 bytes（vendor FF00/0002，大块数据不读取）
```

由 847 字节描述符解析出的 Digitizer 关键布局：

- **Input ReportID `0x54`**：63 字节数据（含 ID 共 64 字节）
  - 10 个重复的 Finger collection，每个 6 字节：
    `byte0 = Tip Switch(bit0) + 7 bit padding`、`byte1 = Contact Identifier`、
    `byte2-3 = X (0..32767)`、`byte4-5 = Y (0..32767)`；
  - byte 60 = **Contact Count (usage 0x0D/0x54)**，byte 61-62 = **Scan Time (0x0D/0x56)**。
  - `--dump` 按此布局解码。
- **Feature ReportID `0x13`**：2 字节数据（含 ID 共 3 字节）
  - byte0 = **Input Mode (0x0D/0x52)**，逻辑范围 0..10，**2 = multi-touch**；
  - byte1 = **Device Identifier (0x0D/0x53)**，本机当前值 11。
- 另有 keyboard report `0x60`、vendor feature `0x44/0x64`（固件数据，探针不读取其值）。
- 设备属性 `RequiresTCCAuthorization=YES`。

### 3.3 `--dump` 实测（`samples/hidprobe-dump.txt`）

```
IOHIDManagerOpen: kr=0x00000000 (kIOReturnSuccess)
已打开 2 个匹配接口；开始监听 input report。
监听 2 秒（--seconds 0 = Ctrl-C 退出）。现在触摸 HOUSI 屏幕……
=== DUMP 汇总 ===
总 input report 数 = 0
未收到任何 input report。可能原因:
  1) 你没有触摸 HOUSI 屏幕（该控制器只在触摸时发报告）；
  2) Input Monitoring 未授权 ...
  3) 设备被其它驱动独占，或接口不在 Digitizer 模式。
```

自动验证时没有物理触摸，所以 0 条报告属于预期。**需要人手触摸屏幕**才能拿到真实报告
（`--dump --seconds 30`，触摸后应看到 `reportID=0x54 len=64 ... contactCount=... fingers=[...]` 行）。

### 3.4 `--setmode` 实测（关键证据）

本机当前 Input Mode 已经是 **2**（多点）。`hidprobe --setmode` 默认只读验证、
跳过写入；`--force` 强制发一次 SetReport；`--mode 1 --restore` 做了“改到 1 → 回读确认 →
恢复 2 → 回读确认”的完整往返测试。

`--setmode --force`（`samples/hidprobe-setmode-force.txt` 节选）：

```
--- STEP 3: 子进程发送 IOHIDDeviceSetReport（Input Mode = 2）---
  写入前: Input Mode=2, Device Identifier=11（来自 IOHIDDeviceGetValue，单次顺序遍历缓存）
  构造 Feature Report: reportID=0x13, 总长度=3 字节（含 ReportID）: 13 02 0b
    buffer[0]=ReportID=0x13, data byte[0] (Input Mode) = 2
  IOHIDDeviceSetReport(Feature, 0x13, buffer[0]=ReportID): kr=0x00000000 (kIOReturnSuccess)
  SetReport 结果: SUCCESS
--- STEP 4: 用全新子进程回读 Input Mode ---
回读尝试 1/6 ...
  READ_MODE mode=2 devid=11
回读: Input Mode = 2, Device Identifier = 11
验证结果: MATCH (期望 Input Mode=2, 实际=2)
RESULT: OK — Input Mode=2 已生效。
```

`--setmode --mode 1 --restore`（`samples/hidprobe-setmode-mode1-restore.txt` 节选）：

```
构造 Feature Report: reportID=0x13, 总长度=3 字节（含 ReportID）: 13 01 0b
SetReport 结果: SUCCESS
回读: Input Mode = 1, Device Identifier = 11
验证结果: MATCH (期望 Input Mode=1, 实际=1)
--- RESTORE: 恢复原 Input Mode = 2 ---
构造 Feature Report: reportID=0x13, 总长度=3 字节（含 ReportID）: 13 02 0b
SetReport 结果: SUCCESS
恢复后 Input Mode = 2
```

（运行前后外部 `hidprobe --read-mode` 均显示 `READ_MODE mode=2 devid=11`，设备状态已还原。）

`--setmode` 的 STEP 0 还会用独立子进程打印 `IOHIDDeviceOpen` / `IOHIDDeviceClose` 返回值
（`samples/hidprobe-setmode-skip.txt` 节选；单独运行可用 `--open-close`）：

```
--- STEP 0: 子进程验证 IOHIDDeviceOpen / IOHIDDeviceClose 返回值 ---
  IOHIDDeviceOpen(digitizer): kr=0x00000000 (success)
  IOHIDDeviceClose(digitizer): kr=0x00000000 (success)
```

> 注意：`--inspect`/`--read-mode`/`--get-report` 这些读取过 GetValue/GetReport 的子进程会
> **跳过 Close**（原因见下方第 3 条），改由 `--open-close` 单独验证 Close 成功。

`--setmode` 同时打印 `IOHIDDeviceGetReport` 原始回读（`samples/hidprobe-get-report.txt`）：

```
IOHIDDeviceGetReport(Feature, reportID=0x13, buffer=3) -> kr=0x00000000 len=3
  raw = 11 0a 00
  注意: 返回的第一字节 0x11 与请求的 reportID 0x13 不一致；本机固件实测对任意 report ID
        都返回同一段 3 字节数据（0x11=Contact Count Maximum 的 report），GetReport 回读不可用于校验 0x13。
```

**重要实测结论（macOS 27.2.1 + 本控制器）：**

1. `IOHIDDeviceSetReport` 的语义与 Apple 文档一致：**buffer 第一个字节必须是 Report ID**，
   后面是 report data。写入 `{0x13, 0x02, 0x0b}` 成功，`{0x13, 0x01, 0x0b}` 也成功并把
   Input Mode 实际改为 1（随后已恢复为 2）。
2. `IOHIDDeviceGetReport` 在本设备上**无视请求的 report ID**，恒定返回 `11 0a 00`
   （即 report 0x11 = Contact Count Maximum 的内容）。因此 Input Mode 的**回读/校验以
   `IOHIDDeviceCopyMatchingElements` + `IOHIDDeviceGetValue(feature element)` 为准**。
3. 修改 Input Mode 会让设备重新枚举；旧 `IOHIDDevice/IOHIDElement` 引用在后续访问时可能崩溃
   （SIGTRAP/SIGSEGV）。所以 `--setmode` 把「读 / 写 / 回读」拆成**独立子进程**，写操作后
   由全新进程重新打开设备回读（`--read-mode`），父进程只做编排。
4. 本机另一个 IOHIDLib 怪癖：**同一进程内对 feature 元素做多轮 GetValue，或多次调用
   `IOHIDDeviceCopyMatchingElements`**，会崩溃；探针因此规定每个子进程只做**一次**元素复制、
   **一轮顺序遍历 GetValue**，并结合 `--read-mode` 子进程获取权威值。也正因如此，
   `--inspect` 不读取 vendor (0xFF00) 大块 feature 数据。

### 3.5 权限（Input Monitoring）

- 设备属性 `RequiresTCCAuthorization=YES`。读取 input report / SetReport 需要
  **系统设置 → 隐私与安全性 → 输入监控（Input Monitoring）** 授权。
- TCC 把授权记在**负责进程**上：通常不是 `build/hidprobe` 自己，而是启动它的终端 App
  （Terminal / iTerm / 宿主 App）。授权后需要**完全退出并重开终端**。
- 本工具会打印实时权限状态（`IOHIDCheckAccess(kIOHIDRequestTypeListenEvent)`）：

  ```
  Input Monitoring 权限 (IOHIDCheckAccess/kIOHIDRequestTypeListenEvent): granted(0)
  ```

  若为 `denied/unknown`，工具会打印授权步骤；也可 `hidprobe --request-access` 主动触发系统提示。
- 若开放失败，`kr=0xE00002E2` 即 `kIOReturnNotPermitted`（无输入监控权限）；本工具也会给出同样提示。
- **辅助功能（Accessibility）** 只有在本工具主动发送 CGEvent 时才需要；本探针只监听和读写
  HID feature，不需要辅助功能。
- 重新编译不会改变 TCC 归属（授权在终端 App 上），但如果把二进制移动/换宿主 App，
  需要重新授权。

---

## 4. 如何把 touchprobe.log 交给 agent 分析

```bash
# 让触控过程跑 30 秒（期间人手触摸 HOUSI 屏幕）
tools/harness/build/touchprobe --duration 30 --logfile evidence/touchprobe.log

# 1) 看有没有任何 direct touch / gesture 事件
grep -cE '\[(EVT|TOUCH|GEST) ' evidence/touchprobe.log
grep -E 'type=directTouch|type=gesture' evidence/touchprobe.log | head -20

# 2) 看每个触摸的归一化坐标和映射候选
grep '\[TOUCH\]' evidence/touchprobe.log | head -40

# 3) 看启动时的屏幕/坐标系信息
sed -n '1,60p' evidence/touchprobe.log

# 4) 如果没有事件: 交叉验证 WindowServer 是否丢弃了 digitizer
log show --last 5m --predicate 'category == "TouchEvents"' --style compact | tail -50
log show --last 5m --predicate 'eventMessage CONTAINS[c] "digitizer"' --style compact | tail -50
```

把 `evidence/touchprobe.log` 整个文件（文本，带时间戳）发给 agent 即可；
`--all-events` 模式会产生大量事件，仅调试时使用。

---

## 5. 验收 / 自检命令

```bash
cd tools/harness
./build.sh                                                   # 1. 一键构建（本机已验证通过）

./build/touchprobe --duration 2 --logfile samples/t.log      # 2. 2 秒自动退出，exit 0
grep -E 'NSScreen.screens|displayID=3|HOUSI\? = YES|duration=' samples/t.log

./build/hidprobe --list                                      # 3. 枚举到 Mouse + Digitizer 两个接口
./build/hidprobe --read-mode                                 # 4. READ_MODE mode=2 devid=11（本机）
./build/hidprobe --open-close                                # 4b. IOHIDDeviceOpen/Close 均 success
./build/hidprobe --get-report                                # 5. GetReport 原始返回 + 固件怪癖说明
./build/hidprobe --setmode                                   # 6. 已是 2 → 只读验证 + 回读 MATCH
./build/hidprobe --setmode --dry-run --force                 # 7. 只打印 13 02 0b，不写设备
./build/hidprobe --setmode --force                           # 8. 真写一次并回读（幂等，设备保持 2）
./build/hidprobe --dump --seconds 5                          # 9. 无触摸时 0 条并给出提示
./build/hidprobe --vid 0x1234 --pid 0x5678 --list            # 10. 无设备时优雅提示（exit 3）

./build/touchprobe --help && ./build/hidprobe --help         # 11. 帮助
```

实测结果：第 2、3、4、5、6、7、8、9、10 步全部按预期通过；第 9 步在无人触摸时
`总 input report 数 = 0`（预期行为）。

> 需要人手参与的最终验证：`touchprobe --duration 30`（触摸 HOUSI 屏）看是否出现
> `EVT/TOUCH`；`hidprobe --dump --seconds 30`（触摸 HOUSI 屏）看是否出现
> `reportID=0x54 ... contactCount=...`。这两个结果将回答“WindowServer 是否仍然丢弃
> 来自 unsupported service 的 digitizer 事件”。

---

## 6. 已知限制与未验证项

1. **本 agent 没有物理触摸设备**：上面的自动运行只验证了程序能编译、能创建窗口、能枚举/读写 HID；
   “触摸后是否收到 AppKit 事件 / input report”必须由 lead/用户实际触摸验证。
2. **GetReport 怪癖**：本机固件对任意 Feature Report ID 都返回 `11 0a 00`。因此本工具用
   `IOHIDDeviceGetValue`（feature element）作为 Input Mode 权威回读；GetReport 原始返回值仍按要求打印。
3. **IOHIDLib 稳定性怪癖**：同一进程内多次 `IOHIDDeviceCopyMatchingElements` / 多轮 feature
   `GetValue` 在本机可能崩溃。本工具通过“子进程 + 单次复制 + 单轮遍历 + 写后新进程回读”规避；
   未来 macOS 修复后这些隔离逻辑仍可安全工作，只是略啰嗦。
4. **修改 Input Mode 会重新枚举设备**：若在 `--write-mode` 恰好中断，设备可能停在非预期模式；
   可用 `hidprobe --setmode --mode 2` 或 `--restore` 恢复，实在异常可重新插拔触控屏。
   本工具默认在模式已等于目标时**不写设备**，避免不必要的模式切换。
5. **vendor feature（0x44/0x64）不读取**：属固件/厂商数据，读取可能触发上述崩溃，且与触控无关。
6. 仅证明 HID/AppKit 两侧“看到什么”，**不解决** displayUUID 关联问题、不加载 DriverKit/HID
   plugin、不注入事件、不加系统权限。
7. `NSScreen.touchCapabilities` 是 macOS 27 新 API；在旧系统上编译时探针回退为 0（有 `#available` 守卫）。
8. global monitor 需要辅助功能/输入监控授权，未授权时只收不到事件。
9. `--dump` 依赖 Input Monitoring；本机当前终端环境已授权，换终端/宿主 App 需重新授权。
10. `touchprobe` 默认写 `WORK/evidence/touchprobe.log`（项目约定），运行时可创建该目录；
    如果只想写别处，用 `--logfile` 指定。

---

## 7. 安全声明

- 工具**未使用 sudo**，**未修改系统文件/系统设置**，**未关闭 SIP**；
- `touchprobe` 只读取屏幕信息与 AppKit 事件；
- `hidprobe` 只读取设备属性/描述符/input report；唯一的写操作是**触控控制器自身的
  Input Mode Feature Report**（可用 `--dry-run` 完全不写），并支持 `--restore` 还原原值；
  本机验证结束时 Input Mode 仍为 2（与开始一致）。

---

## 8. samples/ 索引（本机实际输出）

| 文件 | 内容 |
|---|---|
| `touchprobe-duration2.txt` | `touchprobe --duration 2` 完整 stdout（屏幕信息 + 窗口状态 + SUMMARY） |
| `touchprobe-selftest.log` | 同上日志文件内容（与 stdout 一致，验证追加写） |
| `touchprobe-help.txt` | `touchprobe --help` |
| `hidprobe-list.txt` | `hidprobe --list`：两个接口与 847/75 字节描述符解析 |
| `hidprobe-inspect.txt` | 内部 `--inspect`：feature 元素 dump + 当前 Input Mode + GetReport |
| `hidprobe-read-mode.txt` | 内部 `--read-mode`：`READ_MODE mode=2 devid=11` |
| `hidprobe-get-report.txt` | 内部 `--get-report`：GetReport 原始返回与固件怪癖说明 |
| `hidprobe-setmode-skip.txt` | `--setmode`：STEP 0 Open/Close + 已是 2 → 跳过写入并回读 MATCH |
| `hidprobe-setmode-dryrun-force.txt` | `--setmode --dry-run --force`：只打印 `13 02 0b`，不写设备 |
| `hidprobe-open-close.txt` | 内部 `--open-close`：IOHIDDeviceOpen/Close 返回值 |
| `hidprobe-setmode-force.txt` | `--setmode --force`：真写 `13 02 0b` + 回读 MATCH |
| `hidprobe-setmode-mode1-restore.txt` | `--setmode --mode 1 --restore`：改 1 → 回读 1 → 还原 2 |
| `hidprobe-dump.txt` | `--dump --seconds 2`：无人触摸 → 0 条报告 + 提示 |
| `hidprobe-no-device.txt` | 错误 VID/PID：无设备时优雅提示（exit 3） |
| `hidprobe-request-access.txt` | `--request-access` 授权检查结果 |
| `hidprobe-help.txt` | `hidprobe --help` |
