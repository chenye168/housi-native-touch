# 手势测试清单（G01–G28）分析与结论

来源：用户导出的 `touch-gesture-results.md`（HOUSI 触摸屏逐项实测：✅25 / ⚠️1 / ❌12 / ➖1）

## 结论摘要

- **硬件与多点触控链路无问题**：原始 HID 报告实测 `contactCount` 最高 4（2/3/4 指同时），
  WindowServer 也收到 `contact 0..3 presence` 与 3/4 指 contacts；系统手势引擎在开机时
  正常初始化并已链接到触控处理器（日志：`Initializing system gesture registry` /
  `Linking the system gesture event processor to the touch event processor`）。
- **12 个 ❌ 中，只有 2 个属于“外接触控屏本身”的问题**：
  - **G01 程序坞点击**（已单独立项调查：WindowServer 把触摸投递到了 Dock 连接 0x5C903，
    但 Dock 的 direct-touch 处理对外接 digitizer 不响应；builtIn 分类/主屏切换/私有开关均已排除）。
  - **G15 窗口控制“没有放大”**：macOS 27 的触控适配（窗口按钮放大）依赖直接触控/悬停
    （hover/proximity）与 AppKit direct-touch 投递；本机 AppKit 探针收不到
    `NSEventTypeDirectTouch`，且 HOUSI 面板不提供悬停，系统只把触摸合成为普通点击，
    因此“能点击但无放大”。
- **其余 10 个 ❌ 是触控板/间接手势（trackpad / indirect gestures），不是触屏手势**：
  - 证据 1：SkyLight 中系统手势处理称为 `WSIndirectGestureProcessing` /
    `processIndirectGesturesForEvent` / `indirectGestureProcessor`；
  - 证据 2：这些手势的配置全部属于 `com.apple.AppleMultitouchTrackpad` 域
    （`TrackpadThreeFinger*Gesture`、`TrackpadFourFinger*Gesture`、边缘手势等），
    本机当前 3 指手势设置原本就是 0（关闭）；即使临时置 2 用触控板对照也没生效/不适用于触屏；
  - 证据 3：系统手势的窗口注册日志（`Registering window … for system gestures`）与触摸
    直投递是两条管线：触摸屏走 `BKDirectTouchEventProcessor`，触控板走 Multitouch/trackpad 管线。

## 逐项分类

| 编号 | 测试项 | 结果 | 分类 | 说明 |
|---|---|---|---|---|
| G01 | 轻点：程序坞 | ⚠️ | 外接触屏限制（已调查） | Dock 收到 touchstream 但不响应；详见 REPORT.md §7 |
| G02–G14 | 双击/按住拖移/拖动/快速轻扫/开合/旋转/甩动/拖边缘等 | ✅ | 正常 | 直接触控 + 系统点击/手势合成都正常 |
| G15 | 轻点窗口控制（放大） | ❌ | 触控适配限制 | 依赖 hover/direct-touch 适配；只合成点击，无放大 |
| G16 | 全屏下菜单栏下扫/左上角 | ❌ | 触控板/指针手势 | 全屏隐藏菜单栏是边缘悬停手势，非触屏功能 |
| G17 | 三指上扫 | ❌ | 触控板手势 | `TrackpadThreeFingerVertSwipeGesture`（本机为 0）；indirect gesture 管线 |
| G18 | 三指下扫 | ❌ | 触控板手势 | 同上 |
| G19 | 台前调度点左边缘 | ❌ | 触控板/指针手势 | Stage Manager 左边缘是指针悬停交互 |
| G20 | 右上角下扫 | ❌ | 触控板手势 | 控制中心/通知中心的触控板边缘手势 |
| G21 | 三指左右扫 | ❌ | 触控板手势 | `TrackpadThreeFingerHorizSwipeGesture`（本机为 0） |
| G22 | 拇指+三指外张 | ❌ | 触控板手势 | 触控板四指/五指开合手势；indirect 管线 |
| G23 | 四指捏合 | ❌ | 触控板手势 | `TrackpadFourFingerPinchGesture`；indirect 管线 |
| G24 | 底部上扫 | ❌ | 触控板手势 | Launchpad/显示桌面边缘手势 |
| G25 | 顶部边缘下扫 | ❌ | 触控板手势 | 通知中心边缘手势 |
| G26 | 右边缘左扫 | ❌ | 触控板手势 | 通知中心/今天视图边缘手势 |
| G27 | 灵动岛 | ➖ | 不适用 | 本机无灵动岛（iPhone 功能） |
| G28 | 单指左右轻扫 | ✅ | 正常 | 应用内翻页手势正常 |

## 是否可修复

- **不能通过我们的用户态注入修复**：
  - 触控板系统手势需要走 Multitouch/trackpad 事件管线（`AppleMultitouchDevice` /
    `HSTouchHIDService` 等 Apple 私有服务身份），通用 USB digitizer 不在该管线内；
    要进入该管线只能依赖 Apple 授权（DriverKit HID entitlement / 内置触控硬件），
    与本项目“不关 SIP、不写驱动”的前提冲突。
  - Dock 与触控适配同属系统 UI 的私有 gating（已专项验证无用户态开关）。
- **可选的非原生补偿（需要用户确认）**：
  - 若希望“三指上扫 = 调度中心”等功能可用，可以做一个用户态手势桥：监听 HOUSI 的多指触摸，
    识别手势后发送系统快捷键（如 Ctrl+↑ 调度中心、Ctrl+↓ App Exposé、Ctrl+←/→ 切换空间等）。
    这属于快捷键模拟，不是原生命中手势，需要“辅助功能”授权；不会影响现有原生触控链路。

## 复现/证据文件

- `evidence/multitouch_dump.log`：HID 原始报告，`contactCount=1..4`；
- `evidence/multitouch_ws.log`：WindowServer 收到 `contact 0..3 presence`；
- `evidence/gesture_config_logs.txt`、`evidence/all_gesture_logs.txt`：系统手势注册/链接日志；
- `evidence/dock_test2.log`：触摸投递到 Dock 连接 0x5C903 的证据；
- `REPORT.md` §7：Dock 专项调查。
