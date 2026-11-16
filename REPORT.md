# HOUSI 外接触控屏 macOS 27 原生触控：最终实现与验证报告

日期：2026-11-16  
机器：MacBook Pro 18,1（M1 Pro），macOS 27.2.1（26B131），SIP 开启（未关闭）  
显示器：HOUSI 14" USB-C 触控屏（VID 0x32D7 / PID 0x0001，Digitizer 0x0D/0x04，10 指）  
HOUSI 的 WindowServer 显示器 UUID：`30D985CD-BE3F-4C0E-B055-C5E22CFF2E07`

---

## 0. 最终结论

**已完成。** 不需要关闭 SIP、不需要 Apple entitlement、不需要自写驱动、不需要改显示器固件、**重启后也不需要再拔插或手动运行任何命令**。

真实重启验证（2026-11-16 14:07）：开机后自动完成注入与模式切换，用户实测
**Safari 单指滚动/双指捏合、菜单栏点击均正常**（用户反馈“全部正常”）。
`unsupported service` 日志计数为 0。

---

## 1. 根因（反汇编确认）

WindowServer 只在 digitizer **接入（attach）**时读取该 HID 事件服务的属性 `displayUUID`
（必须是 **CFString** 形式的显示器 UUID）；读不到就丢弃事件：

```
[com.apple.BackBoard:TouchEvents] ignoring digitizer event for display <main>
  from unsupported service: (<BKIOHIDService: … name: USB2IIC_CTP_CONTROL …>)
```

- BackBoard `-[BKHIDDirectTouchEventProcessor _queue_displayUUIDForDigitizerService:]`；
- SkyLight `handle_digitizer_added` 只接受 CFString，且需命中 UUID→WSDisplay 映射；
- 私有 feature flag `backboardd/SupportExternalTouchscreen` 的 fallback 不充分（另需 eventSource==2 与 chamois UUID，SkyLight 侧无 fallback）。

因此最小必要条件：**在 WindowServer attach 之前**让该 HID 事件服务带上
`displayUUID = CFString("30D985CD-BE3F-4C0E-B055-C5E22CFF2E07")`。

---

## 2. 最终实现（三层，全自动）

### 2.1 属性注入（用户态）
把 `displayUUID` 写入 HOUSI digitizer 的 HID 事件服务属性表（IOKit `IORegistryEntrySetCFProperty`；
与 `hidutil property --matching '{"PrimaryUsagePage":13,"PrimaryUsage":4}' --set '{"displayUUID":"…"}'` 等价）。

### 2.2 开机自动 USB 重枚举（解决“启动时序/缓存”问题）
`local.housitouchd`（root LaunchDaemon，约 36KB，C + IOKit）：
- 开机启动时扫描 digitizer 服务，找到即注入 displayUUID；
- 若启动时设备已存在（可能已被 WindowServer 无属性 attach），自动执行**一次 USB 重枚举**
  （IOUSBHostDevice `resetWithError:`，约 70ms），强制 digitizer 服务重建；
- 通过 IOKit 服务出现通知**即时**注入新服务的 displayUUID，抢在 WindowServer attach 之前；
- 之后运行 4 秒，以控制台用户身份通过 LaunchServices 启动模式辅助 App（见 2.3）；
- 同一 boot 只重置一次（`/var/db/housitouchd.lastreset` 记录 boottime）。

### 2.3 登录/重置后自动恢复多点模式
`HOUSITouchHelper.app`（用户态 App，需一次性“输入监控”授权）：
- 通过 IOHIDManager 找到 HOUSI digitizer，`IOHIDDeviceSetReport` 写 feature report 0x13：
  buffer `{0x13, 0x02, 0x00}`（含 ReportID，长度 3；实测必须这样写）；
- 由 root daemon 在 USB 重枚举后以控制台用户身份触发（`launchctl asuser <uid> open -g -j -a …`），
  同时也有用户 LaunchAgent `/Library/LaunchAgents/local.housitouch.setmode.plist` 兜底；
- 实测设置 Input Mode=2 不会导致 HID 服务重建，因此不会破坏已接纳的 displayUUID 关联。

---

## 3. 实测证据（2026-11-16 真实重启，14:07）

```
14:07:22  boot
14:07:40  daemon v3 start（此时 device 尚未出现）
14:07:41  inject displayUUID rid=0x100000f46   （服务出现通知即时注入）
14:07:41.187 WindowServer[414] digitizer attached … displayUUID: 30D985CD-…
14:07:41.188 BKDirectTouchState … displayUUID: 30D985CD-… ; builtIn: NO
14:07:50  HOUSITouchHelper: Input Monitoring access=0 → SETMODE OK mode=2
```

- `unsupported service`：0 条（`evidence/touchevents_final_reboot.txt`）
- 当前 Input Mode：2（`hidprobe --setmode --dry-run`）
- ioreg：`displayUUID="30D985CD-BE3F-4C0E-B055-C5E22CFF2E07"`
- 用户实测：Safari 滚动/捏合、菜单栏点击正常。

---

## 4. 安装/卸载（本机已完成安装；以下用于复现或恢复）

### 4.1 一键安装（需管理员密码）
```bash
# 1) 注入 daemon（root）
sudo $PROJECT/tools/daemon/install.sh
# 2) 多点模式辅助 App + 用户登录任务
sudo $PROJECT/tools/helper/install.sh
# 3) 一次性授权：系统设置 → 隐私与安全性 → 输入监控 → 打开 HOUSITouchHelper
#    （若列表里没有：点 “+”，选择 /Applications/HOUSITouchHelper.app）
```
> 注意：HOUSITouchHelper.app 为 ad-hoc 签名。**不要重新编译/替换/重新签名它**，否则“输入监控”授权会失效、需要重新授权。

### 4.2 卸载（完全回退）
```bash
sudo $PROJECT/tools/helper/uninstall.sh
sudo $PROJECT/tools/daemon/uninstall.sh
# 之后无需重启；系统恢复默认（触控退回鼠标模拟模式）
```

### 4.3 日常检查命令
```bash
launchctl print system/local.housitouchd | head
tail -20 /var/log/housitouchd.log
tail -10 /tmp/housitouch-setmode.log
tools/harness/build/hidprobe --setmode --dry-run   # 期望 mode=2
LC_ALL=C ioreg -r -c AppleUserHIDEventService -l -w0 | grep -a -o '"displayUUID"="[^"]*"'
log show --last 5m --info --debug --predicate 'category == "TouchEvents"' | grep -aiE 'digitizer attached|unsupported service'
```

---

## 5. 已知限制与说明

1. 依赖私有属性 `displayUUID` 与 HID 服务重建时序；macOS 升级后可能失效。
2. HOUSITouchHelper 需要一次性“输入监控”授权（ad-hoc 签名，重签会失效）。
3. 开机时若 daemon 比设备先启动（本次真实重启即如此），靠服务出现通知抢在 WindowServer attach 前注入；
   若某次竞争失败（WindowServer 先 attach 且无 UUID），daemon 的 USB 重枚举兜底会把服务重建再抢一次；
   两条路径都已实测成功。
4. 本次真实重启后用户反馈 Dock 也正常；此前“Dock 点击无反应”可能与此前失败的中间状态有关。若再次出现，
   属 macOS 触控点击合成对 Dock 的偶发限制，不影响 App/菜单栏原生触控。
5. 不修改任何系统文件、不关 SIP；所有改动仅：
   `/Library/LaunchDaemons/local.housitouchd.plist`、`/usr/local/libexec/housitouchd*`、
   `/Library/LaunchAgents/local.housitouch.setmode.plist`、`/Applications/HOUSITouchHelper.app`、
   `/var/db/housitouchd.lastreset`、`/var/log/housitouchd.log`、`/tmp/housitouch-setmode.log`。

---

## 7. Dock 点击问题专项调查（结论：受 macOS 内部限制，暂不可修）

用户反馈：HOUSI 上其他 App（Safari）与菜单栏原生触控正常，但 **Dock 图标点击无任何反应**。
专项实验与证据：

1. **触摸确实投递到了 Dock**：WindowServer TouchEvents 日志显示触摸 hit-test 命中 Dock 的
   SkyLight 连接（Dock main connection `0x5C903`，pid 633），并创建了
   `BKTouchDestination (hitTest) contextID 0x1E00F663; clientConnectionIdentifier: 0x5C903`，
   随后 `touchstreams: start sending isFirstDown:true`，事件坐标 `{544,1017}` 也正落在 Dock 区域。
   → Dock 收到了触摸，不是 WindowServer 拦截问题。
2. Dock 自身包含完整 direct-touch 实现（`DirectTouchEventDecoder`、`DockBarTouchEventHandler`、
   `TouchInteraction`、`_touchHighlightItemAtLocation:` 等字符串/类），说明它有能力处理触控。
3. 已验证不受以下因素影响（均无效）：
   - 把 digitizer 标为 `Built-In=true`（WindowServer 日志变为 `eventSource: builtin; builtIn: YES`）→ Dock 仍无反应；
   - 把 HOUSI 临时设为主显示器（Dock 移到 HOUSI 主屏）→ Dock 仍无反应；
   - Dock 二进制中与触摸相关的 `_os_feature_enabled_impl` 调用只有
     `WindowServer/UnifiedKeyMapping`、`SystemPolicy/NewGatekeeperOverrides`、
     `AppSwitcher/OutOfProcessAppSwitcher`、`PencilAndPaper/SystemNoteTaking`，无触控/外接屏开关。
4. 结论：macOS 27.2.1 的 Dock direct-touch 处理很可能只接受内建触控硬件（与 HID 服务来源/内置端口
   映射等私有条件相关），对外接 USB digitizer 仅接收 touchstream 但不触发 Dock 交互；
   在不修改 Dock/SIP、不获得 Apple 私有能力的前提下无法通过属性注入开启。
5. 可选工作区方案（非原生，未实现）：对落在 Dock 区域的触摸用 CGEvent 合成鼠标点击
   （需要“辅助功能”授权），仅补 Dock 一处交互。若需要可另行实现。

已恢复验证状态：daemon 仅注入 displayUUID（未保留 Built-In 注入），HOUSI 已恢复为副屏，
原生触控（Safari/菜单栏/手势）正常。

---

## 8. 手势清单（G01–G28）分析结论

用户导出的 `touch-gesture-results.md` 中 12 个 ❌ 的完整分类与证据见 **GESTURES.md**。
要点：

- 硬件/多点链路无问题：原始报告 `contactCount` 最高 4，WindowServer 收到 contact 0..3；
  系统手势引擎开机正常初始化并链接到触控处理器。
- 其中仅 **G01 Dock 点击** 与 **G15 窗口控制放大** 属于外接触控/触控适配限制（G01 已专项调查，见 §7）；
- 其余 10 项是**触控板/间接系统手势**（SkyLight `WSIndirectGestureProcessing`，
  配置在 `com.apple.AppleMultitouchTrackpad` 域），不经过外接 digitizer 的直接触控管线，
  在不引入 Apple 私有能力的约束下无法开启；可选的非原生补偿（快捷键桥接）需另行实现，见 GESTURES.md。

---

## 6. 研究与交付物索引

- `analysis/acceptance-check.md`：WindowServer/BackBoard 接纳判定反汇编级分析
- `analysis/disasm/`：关键函数反汇编与定位过程
- `research/hid-channels/FEASIBILITY.md`：HID 插件/DriverKit/虚拟设备/Sidecar 路线与门槛
- `tools/daemon/`：开机自动注入 + USB 重枚举 + 触发模式辅助（`housitouchd`、`housitouch-usbreset`、安装/卸载）
- `tools/helper/`：多点模式辅助 App 与 LaunchAgent（安装/卸载）
- `tools/harness/`：`tsprobe`（AppKit 触控探针）、`hidprobe`（HID 模式/原始报文探针）
- `tools/dscx.c` + `tools/x86cache/`：dyld 共享缓存提取与系统库反汇编环境
- `evidence/`：全部原始日志与实测输出（含最终重启验证 `touchevents_final_reboot.txt`）
