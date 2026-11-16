# 外接触控屏「原生触控」接入可行性调研（macOS 27.2.1 / Apple M1 Pro）

> 调研对象：HOUSI 外接触控屏（USB HID，VID `0x32D7` / PID `0x0001`，UsagePage `0x0D` Usage `0x04`），
> 目标显示器 UUID `30D985CD-BE3F-4C0E-B055-C5E22CFF2E07`。
> 调研方式：本机只读分析（`strings`/`otool`/`plutil`/`codesign`/`ioreg`/dyld 共享缓存）+ 最小 C probe 实测 + 公开资料。
> 全程 **未 sudo、未修改任何系统文件、未关闭 SIP**。

**证据标记约定**

- 【本机已验证】= 在本机 macOS 27.2.1 (26B131) 上实际执行命令/程序得到的输出，证据文件见 `evidence/`。
- 【公开资料】= Apple 官方文档 / 公开仓库记录。
- 【推测】= 由上述证据推导、尚未直接验证的结论，文中给出可验证方式。

---

## 0. 结论先行（TL;DR）

1. **不关闭 SIP、不改系统卷的前提下，"真正让 WindowServer 接受 HOUSI 为某一显示器原生触控"目前没有可用路径。** 缺的那一环是 HID event service 的 `displayUUID` 属性；本机上它既不是内核 ioreg 属性，也不是普通用户进程能写的用户态属性。
2. **HID 插件路线（A）机制上"能匹配第三方 digitizer"，但第三方无法加载自研插件**：搜索路径全部是 Apple 专属目录（唯一可写的候选 `/Library/Apple/...` 被 SIP `restricted` 保护），所有系统插件 `anchor apple` 签名，且 WindowServer 自身带 `flags=0x2000(library-validation)`，非 Apple 签名的代码无法进入其进程。**结论：不可行。**
3. **HIDDriverKit 路线（B）是"正统但昂贵"的路**：需要 Apple 逐个团队审批的 DriverKit HID entitlement（`com.apple.developer.driverkit.family.hid.eventservice`、`.transport.hid` 等）、付费 Apple Developer Program、Xcode/DriverKit SDK；**development 签名 dext 的本地调试需要关闭 SIP**（本机 `systemextensionsctl` 直接拒绝；Apple 文档与 Topti 项目实测均如此）。**结论：可能，但周期长、且能否让 WS 接受 `displayUUID` 未验证（推测可行，需实测）。**
4. **虚拟 HID 设备路线（C）本机实测被拦**：普通用户（uid 501、无 entitlement）调用 `IOHIDUserDeviceCreateWithProperties` 返回 `NULL`；SDK 头文件明文要求 `com.apple.developer.hid.virtual.device`，HIDTouch/Topti 两个公开项目一致记录该 entitlement 只能由 Apple 按团队授予。**结论：需 Apple 授权，无技术旁路。**
5. **Sidecar/Multitouch 旁路（D）证明 Apple 内部就是这么实现"按 displayUUID 注入触控"的**：`/usr/libexec/SidecarDisplayAgent` 内含 `SidecarHID.TouchScreenDigitizer` + `IOHIDUserDeviceCreate` + `Unable to configure displayUUID`，并持有 `com.apple.iohideventsystem.server` 等私签 entitlement；`replayd` 用 `BKSTouchStream`（`com.apple.backboardd.touchStreams`）按 displayUUID 消费触控流。**但所有门槛都是 Apple 私签 entitlement，无公开申请通道。结论：第三方不可用。**
6. **KEXT 路线不可行**：kext 自 Big Sur 起被 Apple 弃用（官方支持页），Apple 芯片安装 kext 必须把启动安全策略降为 Reduced Security；本机 SIP + Authenticated Root 均开启。**结论：不可行且不建议。**
7. **短期唯一"可落地"方案仍是用户态事件注入**（CGEvent/NSEvent，HIDTouch/MacTouch 的做法）：零系统改动、可回退，但拿不到 `NSTouch`/`NSEventTypeDirectTouch` 原生语义，只能模拟鼠标/滚轮/手势。
8. **推荐优先级**：① 用户态注入（立即可用）→ ② 走 Apple entitlement 申请（DriverKit HID 或 virtual HID device，二者都是"申请-审批-签名"路线）→ ③ 向 Apple 提交 external touch display 需求 / 关注 `SupportExternalTouchscreen` 特性 → ✗ 自研插件/kext/改系统。

---

## 1. 本机基线与已确认事实

### 1.1 环境

```text
$ sw_vers
ProductName:            macOS
ProductVersion:         27.2.1
BuildVersion:           26B131
$ csrutil status
System Integrity Protection status: enabled.
$ csrutil authenticated-root status
Authenticated Root status: enabled
$ uname -m
arm64
```

（证据：本会话命令输出）

### 1.2 HOUSI 在本机的 HID 事件服务状态【本机已验证】

`ioreg`（服务级）显示 HOUSI 的 digitizer 服务已被 `AppleUserHIDEventDriver`（系统自带
`com.apple.AppleUserHIDDrivers` dext）创建为 `AppleUserHIDEventService`：

```text
VendorID = 13015 (0x32D7)   ProductID = 1
PrimaryUsagePage = 13 (0x0D)   PrimaryUsage = 4 (0x04)
Transport = USB   Product = "USB2IIC_CTP_CONTROL"
DisplayIntegrated = Yes
IOServiceDEXTEntitlements = "com.apple.developer.driverkit.family.hid.eventservice"
IOUserClientCreator = "pid 415, WindowServer"     # WindowServer 直接打开了该 event service 的 user client
```

证据文件：`evidence/09-ioreg-housi.txt`（原始摘录见 `../evidence/ioreg_hideventservice.txt` 第 60–135 行）。

**关键点：内核服务树里 `displayUUID` 出现次数为 0**：

```text
$ ioreg -w0 -l | grep -ci displayUUID
0
```

即 `displayUUID` 不是内核 HID 设备属性，而是 **用户态 IOHIDEventSystem 服务属性**（由插件/宿主进程写入，
WindowServer 在 digitizer attach 时读取）。这与公开研究结论一致（见 §5 参考 [1]）。

只读枚举确认该服务在用户态事件系统中可见，但没有 `displayUUID`：

```text
$ ./probes/svcprobe        # 仅 Copy* API，不 set 任何属性
client = 0x104a5ad90
services = 0x104a5c420 count=182
== service[51] target=1 ==
  VendorID = +13015, ProductID = +1
  PrimaryUsagePage = +13, PrimaryUsage = +4
  Product = "USB2IIC_CTP_CONTROL", Transport = "USB"
  displayUUID = (null)
```

证据文件：`evidence/04-svcprobe.txt`。
（注意：`IOHIDEventSystemClientCreateSimpleClient` 对可读属性有白名单，所以 `(null)` 不能单独作为"服务没有该属性"的
决定性证据；决定性证据是 WindowServer 自己的日志，见下。）

### 1.3 WindowServer 侧的表现

- WindowServer 日志：`BKDirectTouchState ... touchScreen; builtIn: NO; displayIntegrated: YES; ...`，
  说明 WS 确实为 HOUSI 建立了直接触控状态机；
- 但每个事件都报错丢弃（仓库根 `../evidence/unsupported_hits.txt` 中 2322 条）：

```text
WindowServer[415] [com.apple.BackBoard:TouchEvents] ignoring digitizer event for display <main>
  from unsupported service: (<BKIOHIDService: ...; primaryUsagePage: 0xD; primaryUsage: 0x4;
  name: USB2IIC_CTP_CONTROL; transport: USB; serviceStatus: matched>)
```

该错误字符串经二进制定位在 **`BackBoardHIDTouchEventProcessor.framework`**（见 §5.2、`evidence/05-imgfind.txt`），
同处还有 `touch services cannot match nullDisplay`、`no currently matched services for %@`
等字符串 —— 即：**服务本身 match 成功了，但没有被"匹配到某块显示器"**，这正是缺 `displayUUID` 的表现。

---

## 2. 路线 A：HID ServicePlugin / ServiceFilter 扩展机制

### 2.1 谁加载？搜索什么目录？【本机已验证 + 强推断】

对 dyld 共享缓存与本机镜像做字符串定位后，**插件加载代码位于 `IOKit.framework`（IOHIDEventSystem 服务端实现）**：

```text
$ ./probes/imgfind
HIT  pattern=/System/Library/HIDPlugins
     image=/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit seg=__TEXT off=0xcc7dc
HIT  pattern=ServicePluginDebug
     image=.../IOKit.framework/Versions/A/IOKit off=0xc41e4
HIT  pattern=HIDServicePlugin
     image=.../IOKit.framework/Versions/A/IOKit off=0xc43f0
HIT  pattern=com.apple.iohideventsystem.server
     image=.../IOKit.framework/Versions/A/IOKit off=0xc3d7d
```

`IOKit.framework` 中同时存在（`evidence/01-plugin-search-paths.txt`）：

```text
/System/Library/HIDPlugins
/AppleInternal/Library/HIDPlugins
/usr/appleinternal/lib/HIDPlugins
/Library/Apple/System/Library/HIDPlugins
/System/Library/HIDPlugins/SessionFilters   (及同样 4 前缀)
/System/Library/HIDPlugins/ServiceFilters   (及同样 4 前缀)
/System/Library/HIDPlugins/ServicePlugins
/System/Library/HIDPlugins/ConnectionPlugins
Found plugin %s.
kIOKitPersonalitiesKey      Start Matching Exclude Level
NSPrincipalClass            OBJC_CLASS_$_%@
initWithService:            propertyForKey:client:      setProperty:forKey:client:
HIDServicePlugin            HIDServiceFilter            HIDSessionFilter        HIDConnectionPlugin
HIDPropertiesRequiredForMatching
```

补充：任务里点名要查的两个位置——`/System/Library/Extensions/IOHIDFamily.kext` 与
`/System/Library/PrivateFrameworks/HID.framework`——**都不是插件加载器所在**：前者在 Apple Silicon 上只剩
`Info.plist`/`_CodeSignature`（可执行体在 kernelcache 里，本机无法直接 strings）；后者已用 `imgfind` dlopen 后扫描，
**没有** `HIDPlugins` 路径字符串（`evidence/05-imgfind.txt`）。插件加载代码在本机收敛在 **`IOKit.framework`**。

**加载者【强推断】**：凡是 host `IOHIDEventSystem` **服务端**的进程都会加载这些插件。本机同时有
`hidd`（pid 486）与 `WindowServer`（pid 415），二者都持有 `com.apple.iohideventsystem.server` entitlement
（`codesign -d --entitlements -`，见 `evidence/06-windowserver-codesign.txt`）；WindowServer 进程日志里
存在 `[com.apple.Multitouch:Plugin] [TP] Dispatching digitizer event ...`（10687 条，见仓库根 `../evidence/*.txt`），
说明至少 **SessionFilter/Multitouch 插件代码在 WindowServer 进程内运行**。未使用调试器直接观测 dlopen
（无 root/调试器），"hidd + WindowServer 各自加载"是二进制 + 日志 + ioreg 的强推断。

**搜索目录没有第三方位置【本机已验证】**：

```text
$ ls -d /Library/HIDPlugins ~/Library/HIDPlugins
No such file or directory
$ ls -ldO /Library/Apple /Library/Apple/System/Library
drwxr-xr-x@ root wheel restricted  ... /Library/Apple
drwxr-xr-x  root wheel restricted  ... /Library/Apple/System/Library
```

- 搜索列表中的 4 个根目录里，3 个位于 **Sealed System Volume**（`/System`、`/AppleInternal`、`/usr/appleinternal`），
  1 个（`/Library/Apple/System/Library`）位于 **SIP `restricted`** 目录，普通 root 都不可写。
- **不存在** `/Library/HIDPlugins`、`~/Library/HIDPlugins` 之类第三方目录。

### 2.2 插件签名 / library validation【本机已验证】

```text
$ for p in ServiceFilters/DFRTouchServiceFilter.plugin ServicePlugins/HSTouchHIDService.plugin \
           ServicePlugins/DualSenseHIDServicePlugin.plugin SessionFilters/MultitouchSessionFilter.plugin; do
    codesign -d -r- /System/Library/HIDPlugins/$p | tail -1; done
designated => identifier "com.apple.hid.DFRTouchServiceFilter" and anchor apple
designated => identifier "com.apple.hid.HSTouchHIDService" and anchor apple
designated => identifier "com.apple.GameController.HID.DualSense" and anchor apple
designated => identifier "com.apple.MultitouchSessionFilter" and anchor apple

$ codesign -dvvv /System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/Resources/WindowServer
CodeDirectory v=20400 ... flags=0x2000(library-validation) ...
Authority=macOS Software Signing
```

=> 所有系统 HID 插件都是 **Apple 平台签名（anchor apple）**；WindowServer 带 **library validation**
（`flags=0x2000`），即使有人能把非 Apple 签名的 bundle 放进某个可写路径，**也无法被 WS 加载**。
（`hidd` 本身 `flags=0x0`，但它的插件目录同样受 SIP 保护；且正常路径上 WindowServer 才是触控事件的消费方。）

### 2.3 插件如何匹配设备（以三个真实 Info.plist 为例）【本机已验证】

```text
# ServiceFilters/DFRTouchServiceFilter.plugin —— 匹配任意 13/4 digitizer，无 VID 限制
"Matching" => { "DeviceUsagePairs" => [ { "DeviceUsagePage" => 13, "DeviceUsage" => 4 } ] }
"NSPrincipalClass" => "DFRTouchServiceFilter"

# ServicePlugins/HSTouchHIDService.plugin —— 只匹配内置触控硬件
"Matching" => { "IOKitPersonalities" => { "Default" => {
    "IOPropertyMatch" => { "HSTouchHIDService" => true },
    "IOProviderClass" => "AppleMultitouchDevice" } } }
"NSPrincipalClass" => "HSTouchHIDService"

# ServicePlugins/DualSenseHIDServicePlugin.plugin —— 按事件服务属性匹配
"Matching" => { "IOPropertyMatch" => { "GameControllerCategory" => "ps5", "GameControllerSupport" => true } }
```

完整输出见 `evidence/02-plugin-plists-signing.txt`。

**含义**：匹配机制本身支持 `DeviceUsagePairs`（usage pair）与 `IOPropertyMatch`（任意服务属性），
所以 **一个 Apple 签名的 ServiceFilter 完全可以匹配到 HOUSI（13/4），事实上 `DFRTouchServiceFilter` 的匹配条件
就覆盖它**。瓶颈不是"能不能匹配"，而是"第三方能不能加载自己的插件"。

### 2.4 插件能不能提供 `displayUUID` / 接管事件？【本机已验证 + 推测】

- **机制上能**：`HIDServicePlugin` 协议含 `initWithService:` / `propertyForKey:client:` / `setProperty:forKey:client:`；
  **`HSTouchHIDService` 就是唯一会写 `displayUUID` 的系统 HID 插件**，其字符串里有完整证据链：

  ```text
  displayUUID
  displayServiceMatched
  Notified of matched display service
  Found display UUID in display service: %@
  Could not retrieve display UUID
  builtInPortNumber=
  ```

  （`evidence/08-hstouch-dfrtouch.txt`）——它从"关联的 display service"里取 UUID，再作为 HID event service 属性设上去。
- **但它只匹配 `AppleMultitouchDevice`**（内置触控/触控板），第三方 USB HID digitizer 不在其匹配范围。
- **`DFRTouchServiceFilter` 的匹配条件覆盖 13/4（按 Info.plist 应当会 attach 到包括 HOUSI 在内的 digitizer，实际 attach 未单独验证），但它是纯透传/打日志的 filter**，
  二进制里只有 `Attaching to service` / `Filter activated` 之类日志与 `HIDServiceFilter` 协议方法，
  **完全没有 `displayUUID` 逻辑**（同上证据文件）。
- **不能"借用" HSTouchHIDService**：它要求 provider class 是 `AppleMultitouchDevice` 且带 `HSTouchHIDService=true`
  属性；给 HOUSI 的服务树写这个属性需要 root/kext/dext，且 provider class 也不可能变成 `AppleMultitouchDevice`。

**A 结论：不可行。** 第三方无法把自己的插件放进被搜索的路径（SIP protected / Sealed System Volume），
也无法绕过 WindowServer 的 library validation；Apple 现有插件里没有能给外接 USB digitizer 提供 `displayUUID` 的。

---

## 3. 路线 B：HIDDriverKit（dext）

### 3.1 需要的 entitlement（官方文档）【公开资料】

| entitlement | 官方说明（摘要） | 官方链接 |
|---|---|---|
| `com.apple.developer.driverkit` | DriverKit 驱动基础 entitlement | https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.driverkit |
| `com.apple.developer.driverkit.family.hid.eventservice` | 驱动向系统提供 HID event service | https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.driverkit.family.hid.eventservice |
| `com.apple.developer.driverkit.family.hid.device` | 提供 HID device（provider） | https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.driverkit.family.hid.device |
| `com.apple.developer.driverkit.transport.hid` | 与 HID 设备通信 | https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.driverkit.transport.hid |
| `com.apple.developer.driverkit.transport.usb` | USB 传输（若直接做 USB provider） | 同 DriverKit 文档族 |
| `com.apple.developer.system-extension.install` | 宿主 App 激活系统扩展 | https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.system-extension.install |
| `com.apple.developer.hid.virtual.device` / `com.apple.developer.driverkit.family.hid.virtual.device` | 创建虚拟 HID 设备（C 路线也要） | https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.hid.virtual.device ；https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.driverkit.family.hid.virtual.device |

官方 HIDDriverKit 总览：https://developer.apple.com/documentation/hiddriverkit （“Develop drivers for
human-interface devices, such as keyboards, pointing devices, and digitizers like pens and touch pads.”）

### 3.2 Apple 审批入口与账号要求【公开资料】

- 申请文档：**Requesting Entitlements for DriverKit Development**
  https://developer.apple.com/documentation/driverkit/requesting-entitlements-for-driverkit-development
  原文要点：
  - “DriverKit services communicate with sensitive parts of the system … the system loads only drivers that have a
    valid set of entitlements.”
  - “request the entitlements you need from Apple … **Apple ties any requested entitlements to your development
    team’s profile**”；
  - “While waiting for Apple to grant your entitlement requests, you can continue to develop and test your drivers on
    your local systems.”（本地开发需要关闭校验，见 3.3）
- 申请表单入口（文档中“request an entitlement”链接的落地页）：
  `https://developer.apple.com/contact/request/system-extension/`
  本机实测：`HTTP/1.1 302 Found → Location: https://idmsa.apple.com/IDMSWebAuth/signin.html?path=%2Fcontact%2Frequest%2Fsystem-extension%2F...`
  ——**必须先登录 Apple Developer 账号**（`curl -sSI` 输出可复现）。
- 账号：需要 **Apple Developer Program 会员**（付费，公开资料为 99 USD/年；加入后获得 Team ID 才能把
  entitlement 授予到 profile）：https://developer.apple.com/programs/
- 系统扩展必须打包在 App 的 `Contents/Library/SystemExtensions` 下，App 需安装在 `/Applications`，
  激活时系统校验代码签名与 entitlement 是否与团队匹配：
  https://developer.apple.com/documentation/systemextensions/installing-system-extensions-and-drivers

### 3.3 本机加载 development-signed dext 的限制【本机已验证 + 公开资料】

- **SIP 开启时无法启用 developer mode**：

  ```text
  $ strings -a /usr/bin/systemextensionsctl | grep -i integrity
  At this time, this tool cannot be used if System Integrity Protection is enabled.
  Please remember to re-enable System Integrity Protection!
  $ csrutil status
  System Integrity Protection status: enabled.
  ```

- Apple 官方 **Debugging and testing system extensions** 文档（https://developer.apple.com/documentation/driverkit/debugging-and-testing-system-extensions）原文：
  - “To allow development of your app outside of these directories, use the `systemextensionsctl` command-line tool to
    enable developer mode.”
  - “**System Integrity Protection (SIP) in macOS prevents unauthorized code from running on your system. Xcode doesn’t
    notarize apps or system extensions during the normal development cycle, so disabling SIP bypasses the notarization
    checks** that the system normally performs …”
  - 调试还需 `lldb`（root）附加。
- 第三方项目实证（Topti，见 §5 参考 [3]）：`docs/decisions/0002-cgevent-tablet-output.md` 原文：
  “DriverKit `IOUserHIDDevice` dext: installed and dispatched reports, but no Quartz events reached apps;
  on-demand control needs another unapproved entitlement. **Testing it required SIP changes and reboots.**”
- 本机已有系统扩展列表（OBS 摄像头扩展、Proxifier、Tailscale）说明**用户可批准 Apple/Developer ID 签名的正式扩展**，
  但这与"开发签名 + developer mode"是两条不同通道；后者在本机 SIP 开启时不可用。

### 3.4 本机 SDK / 工具链证据【本机已验证】

```text
$ ls /Library/Developer/CommandLineTools/SDKs/
MacOSX.sdk  MacOSX26.5.sdk  MacOSX26.sdk  MacOSX27.2.sdk  MacOSX27.sdk      # 无 DriverKit*.sdk
$ ls -d /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/System/Library/Frameworks/DriverKit.framework
.../DriverKit.framework            # CLT SDK 内有 DriverKit 基础框架与 102 个头/iig
$ ls -d .../MacOSX.sdk/System/Library/Frameworks/HIDDriverKit.framework
No such file or directory           # 但**没有** HIDDriverKit.framework
$ clang -target arm64-apple-driverkit27.2 -isysroot <MacOSX.sdk> -c t.c
clang: warning: using sysroot for 'macOS 27.2' but targeting 'arm64-apple-driverkit27.2.0' [-Wincompatible-sysroot]
```

=> 结论要修正背景里的说法：**CLT 的 MacOSX.sdk 里其实带 DriverKit.framework 头**（可编译部分核心类型），
但**没有独立的 DriverKit SDK、没有 HIDDriverKit.framework**（`IOUserHIDEventDriver` 等 HID dext 类在
`HIDDriverKit.framework` 内），且没有 Xcode 的 DriverKit 打包/签名流程。要正经开发 HID dext **仍需要 Xcode + DriverKit SDK**。
（证据：`evidence/10-systemext-driverkit-kext.txt`）

### 3.5 KEXT 路线已不可行【本机已验证 + 公开资料】

- Apple 官方支持页 *Deprecated Kernel Extensions and System Extension Alternatives*：
  “kernel extensions have been deprecated. Starting with macOS Big Sur, macOS releases no longer load kernel extensions
  that use deprecated kernel programming interfaces (KPIs) by default.”
  https://developer.apple.com/support/kernel-extensions/
- Apple 芯片安装 kext 必须把启动安全策略降为 **Reduced Security**：
  “Before an extension (also known as a kernel extension or kext) can be installed on a Mac computer with Apple
  silicon, the security policy must be changed to Reduced Security.”
  https://support.apple.com/guide/mac-help/mchl768f7291/mac
- 本机：`kextload`/`kextutil`/`kmutil` 仍在，但 SIP + Authenticated Root 均开启、无 passwordless sudo；
  且 kext 需要 root 安装 + 重启 + 降低启动安全 → **不可行，也不建议**（每次 OS 升级都可能失效，安全风险高）。

### 3.6 B 路线能否解决 `displayUUID`？【推测】

HIDDriverKit 的 `IOUserHIDEventService` 可以在事件服务侧设置属性（`IOHIDEventService` 暴露
`kIOHIDEventServicePropertiesRequiredForMatching` 等），理论上可在 `Start` 时 `setProperty("displayUUID", ...)`；
但 **公开文档没有任何 `displayUUID` 键的说明，也没有第三方成功案例**（HIDTouch/glasses666 均只到
“用户态 IOHIDServiceClientSetProperty 不持久”这一层）。因此：
**B 路线"可能"能解决，但必须实测**——用 dext 在 `IOUserHIDEventService`/`IOHIDEventService` 侧设置
`displayUUID`，观察 WindowServer 日志中 `from unsupported service` 是否消失。此验证只有在 **拿到 entitlement + 关 SIP 的测试机** 上才能做。

**B 结论：技术可行路线中"最正统"的一条，但门槛 = Apple 审批 + 付费账号 + 关 SIP 开发环境 + 未知的 displayUUID 接受度。**

---

## 4. 路线 C：虚拟 HID 设备（IOHIDUserDevice / CoreHID）

### 4.1 本机实测：普通用户创建虚拟 HID 设备被拒【本机已验证】

最小 C 程序 `probes/virtdev.c`（构造 13/4 digitizer 报告描述符，设置 VID/PID/usage/Built-In/**displayUUID** 后调用）：

```text
$ id -u
501
$ codesign -d --entitlements - ./probes/virtdev
Executable=.../virtdev          # 无任何 entitlement（ad-hoc）
$ ./probes/virtdev
IOHIDUserDeviceCreateWithProperties -> 0x0 (errno=3 No such process)
RESULT: BLOCKED (no entitlement / no permission)
```

证据：`evidence/03-virtual-hid-device-test.txt`。
（`errno=3` 只是残留值；决定性信号是返回 `NULL`。本机 `log show` 被沙箱拒绝，未能取到内核/服务端拒绝原因。）

### 4.2 官方要求与公开项目记录【公开资料】

- SDK 头文件 `IOKit/hidsystem/IOHIDUserDevice.h` 原文（本机 SDK 内）：
  > “The IOHIDUserDeviceRef represents a virtual IOHIDDevice. In order to create the device, the entitlement
  > **`com.apple.developer.hid.virtual.device`** is required to validate the source of the device.”
- 官方 entitlement 页：https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.hid.virtual.device
  以及 DriverKit 变体 `com.apple.developer.driverkit.family.hid.virtual.device`（“An entitlement that lets an app
  create and manage virtual HID devices.”）。
- **CoreHID**（macOS 15+）同样提供虚拟设备 API：本机 SDK 的 Swift 接口里有
  `MacOSX.sdk/System/Library/Frameworks/CoreHID.framework/Modules/CoreHID.swiftmodule/arm64e-apple-macos.swiftinterface`：
  `public actor HIDVirtualDevice { public init?(properties: ...) ... }`
  ——`init?` 是可失败初始化，同样受 entitlement 门控（官方文档：https://developer.apple.com/documentation/corehid/hidvirtualdevice ）。
- **HIDTouch README**（https://github.com/koshi545/HIDTouch ，本机副本 `../evidence/hidtouch_README.md`）：
  > | `IOHIDUserDevice` virtual digitizer | ❌ blocked on an Apple-issued entitlement |
  > “`IOHIDUserDeviceCreate` requires the restricted entitlement `com.apple.developer.hid.virtual.device`, which
  > **Apple grants per developer team** and which **must be embedded in a provisioning profile**. Ad-hoc and
  > self-signed builds cannot obtain it.”
- **Topti**（https://github.com/Yukaii/topti ）2026-04 至 06 的实证（`docs/decisions/0002-cgevent-tablet-output.md`）：
  - CoreHID `HIDVirtualDevice`（**restricted entitlement, granted**）：Quartz 能收到 tablet 事件，但压力值恒为 0
    （“WindowServer drops pressure when it promotes the device to tablet events”）；
  - 结论：他们最终 **放弃虚拟 HID/DriverKit，改用 CGEvent 注入**（“They are retired”）。

### 4.3 C 结论

- **无 entitlement 时 100% 被拒**（本机实测 + SDK 明文 + 两个公开项目一致）。
- 有 entitlement 时"虚拟 digitizer 可被系统接受"已被 Topti 在 tablet/stylus 方向间接证明；但
  **"虚拟 digitizer + `displayUUID` → 外接屏原生触控"这条组合没有任何公开成功案例**（Sidecar 是 Apple 私有的等价实现，见 §5）。
- 因此 C 路线的真实状态：**"可申请、需 Apple 授权、目标效果未验证"**。

---

## 5. 路线 D：Sidecar / Multitouch / BKTouch 旁路（Apple 内部机制逆向）

### 5.1 WindowServer 内确实有 Multitouch 插件路径在跑【本机已验证（日志命中）+ 强推断（插件归属）】

```text
$ grep -h 'com.apple.Multitouch' ../evidence/*.txt | wc -l
10687
WindowServer[469] [com.apple.Multitouch:Plugin] [TP] Dispatching digitizer event with 2 children,
  _eventMask=0x63 _childEventMask=0x62 Cancel=0 Touching=1 inRange=1
```

说明：WindowServer 进程内有一套 Multitouch/Plugin 路径在**处理 digitizer 事件并向下派发**（`[TP]`）。

### 5.2 触控事件处理器（`BackBoardHIDTouchEventProcessor`）的内幕【本机已验证】

对该框架的 `__TEXT` 做字符串提取（`evidence/05-imgfind.txt` + 直接 dump），关键字符串：

```text
displayUUID     clients     com.apple.backboardd.touchStreams
BKTouchStreamServer.m     BKDigitizerTouchStreamCreate
ignoring digitizer event for display %{public}@ from unsupported service: (%{public}@):%{public}@
digitizer attached (sender %llX) service:%{public}@
*** already have a main display digitizer / *** acquired main display digitizer
touch services cannot match nullDisplay
no currently matched services for %{public}@
Not supported: accessibility digitizer for external display (%{public}@)
cancel -- digitizer did disappear / BKDirectTouchState / BKIOHIDService / BKGestureProcessingWrapper
```

可知：digitizer 服务 → 按某个 key（`displayUUID`）匹配"当前显示器"；匹配不到就落到
`unsupported service` / `nullDisplay` 分支。**这与 §1.3 的日志完全对应，说明 HOUSI 缺的就是这个关联 key。**

### 5.3 Sidecar 的实现：Apple 自己怎么"按 displayUUID 注入触控"【本机已验证】

`/usr/libexec/SidecarDisplayAgent`（2.1 MB，真实二进制）内嵌 Swift 模块 `SidecarHID`：

```text
$ strings -a /usr/libexec/SidecarDisplayAgent | grep -E 'SidecarHID|IOHIDUserDeviceCreate|displayUUID|TouchScreenDigitizer'
SidecarHID.TouchScreenDigitizer / SidecarHID.Multitouch / SidecarHID.UserDevice / SidecarHID.VirtualService
TouchScreenDigitizer        configureTouchScreenDigitizer(for:with:)
IOHIDUserDeviceCreate: %llx
Unable to configure displayUUID for %{public}u
Display doesn't support MacOSCompatibleTouchReports
DisplayIntegrated   PrimaryUsagePage   Transport
```

entitlements（`codesign -d --entitlements -`，节选）：

```text
com.apple.iohideventsystem.server            = true
com.apple.private.hid.client.event-dispatch  = true
com.apple.private.hid.client.event-monitor   = true
com.apple.hid.manager.user-access-device     = true
com.apple.private.tcc.allow                  = [ kTCCServiceListenEvent, kTCCServicePostEvent ]
```

**解读**：Apple 的 Sidecar 触控 = **在特权进程里创建虚拟 HID digitizer（IOHIDUserDeviceCreate）→ 给它设置
`displayUUID`（“Unable to configure displayUUID” 是失败日志）→ 系统按 displayUUID 把触控投到 Sidecar 显示器**。
这正是我们要的机制，但它的门槛是 **Apple 私签 entitlement（`com.apple.iohideventsystem.server` 等）+ 虚拟 HID 设备 entitlement**，
没有任何公开申请通道。

### 5.4 replayd 的 BKTouchStream：只"消费"不"注入"【本机已验证 + 推断】

```text
$ strings -a /usr/libexec/replayd | grep -E 'SCTouchStream|BKSTouchStream|displayUUID'
SCTouchTracker: BKSTouchStream init failed contextID=0x%x displayUUID=%@
SCTouchTracker: touchStream=%@ connectionID=%u windowID=%u contextID=0x%x displayUUID=%@
-[SCTouchStreamSystemBackend startForDisplay:eventsPendingHandler:]
$ codesign -d --entitlements - /usr/libexec/replayd | grep -E 'touchStreams|virtualDisplay'
com.apple.backboardd.touchStreams   = true
com.apple.backboardd.virtualDisplay = true
```

- `BKSTouchStream`（`BackBoardServices` 里有 `BKSTouchStream.m`；服务端在 `BackBoardHIDTouchEventProcessor`
  的 `BKTouchStreamServer.m`）是 **"按 displayUUID 订阅触控事件流"的消费者 API**，replayd 用它做录制/触控可视化，
  **不是注入 API**。
- `com.apple.backboardd.touchStreams`、`com.apple.backboardd.virtualDisplay` 等是一整族 **Apple 私签 entitlement**
  （`BackBoardHIDEventFoundation` 里可 dump 到完整列表：`cancelsTouches`、`transferTouches`、`detachTouches`、
  `virtualDisplay`、`touchDeliveryObservation`、`replayClone`、`displayArrangement` …），无公开申请入口。
- SkyLight 里还有 `BKTouchEvents`、`BKTouchEventServer`、`BKSTouchEventService`、`BKTouchDeliveryObservation`
  等类，同样属于私有 BackBoard 事件投递体系。

### 5.5 调用路径猜测（Mach-O 符号 / 字符串级别）【推测 + 已验证符号存在】

**Sidecar（Apple 私有，等价于"我们想要的能力"）**

```text
SidecarDisplayAgent 进程（持有 com.apple.iohideventsystem.server 等私签 entitlement）
  └─ SidecarHID.TouchScreenDigitizer.initializeTouchScreenDigitizer()
       └─ IOHIDUserDeviceCreateWithProperties(...)          // 日志字符串 "IOHIDUserDeviceCreate: %llx"
            └─ 构造 13/4 TouchScreenDigitizer 描述符与 VID/PID 属性
       └─ configureTouchScreenDigitizer(for:with:)
            └─ 通过 IOHIDEventSystemClient / IOHIDServiceClientSetProperty
               写 "displayUUID" = 目标显示器 UUID
               └─ 失败时打印 "Unable to configure displayUUID for %{public}u"
WindowServer 侧
  └─ BackBoardHIDTouchEventProcessor:
       "digitizer attached (sender %llX) service:%{public}@" → 建立 BKDirectTouchState
       按 service 的 displayUUID 匹配显示器（SkyLight 字符串 "displayUUID" / "handle_digitizer_added"）
       匹配失败 → "no currently matched services" / "from unsupported service" 丢弃
```

**replayd（消费侧，不是注入侧）**

```text
replayd（持有 com.apple.backboardd.touchStreams / virtualDisplay 等私签 entitlement）
  └─ -[SCTouchStreamSystemBackend startForDisplay:eventsPendingHandler:]
       └─ BKSTouchStream（BackBoardServices，BKSTouchStream.m）
            └─ 服务端 BKTouchStreamServer.m（BackBoardHIDTouchEventProcessor）
                按 displayUUID 订阅触控事件流 → 回调 eventsPendingHandler
```

**结论性问题**：“是否存在向指定 `displayUUID` **注入**合成 digitizer 事件的私有 API？”
本次在 SkyLight / BackBoardServices / BackBoardHIDTouchEventProcessor / BackBoardHIDEventFoundation 的字符串中
**没有找到**任何"注入/投递合成触控"的入口；能找到的 `BKTouch*` / `BKSTouchStream` 全部是**消费/观测**侧
（另见 §5.4 的 entitlement 清单：`cancelsTouches`/`transferTouches`/`detachTouches` 也是"取消/转移已有触控"，
不是凭空注入）。Apple 自己的"注入"实质是 **先造一个虚拟 HID digitizer 服务，再让服务带 `displayUUID`**，
所以门槛回到 C 路线（virtual HID device entitlement + 私有 HID entitlement）。

### 5.6 CGVirtualDisplay 能否曲线救国？【推测】

`CGVirtualDisplay` 是 CoreGraphics 的**公开**虚拟显示器 API（macOS 13+），可以建出带 UUID 的虚拟显示器；
但**没有任何公开 API 把 digitizer/触控绑定到虚拟显示器**，绑定键仍是 HID event service 的 `displayUUID`。
所以 CGVirtualDisplay 只解决"显示器"，不解决"触控关联"，**不能**绕开 §4 的 entitlement 门槛。

### 5.7 D 结论

**存在（Apple 内部在用），但第三方不可用。** 其门槛与 C 路线同源（虚拟 HID device + 私有 HID/BackBoard
entitlement），区别只是 Apple 的 Sidecar agent 用的是私签版本。**除非获得 Apple 授权，D 路线无路。**

---

## 6. 结论表：可行性与风险打分

评分：5 = 最可行/最低风险；0 = 不可行。（"可行性"综合技术可行性 + 授权门槛 + 本机可实施性）

| # | 路线 | 可行性 | 需 Apple 授权 | 需关 SIP | 需重启/重装 | 可回退 | 结论 |
|---|---|---|---|---|---|---|---|
| A1 | 自研 HID ServicePlugin / ServiceFilter | **0** | 否（但需 Apple 平台签名） | 是（且需改写 Sealed System Volume / 关闭 Authenticated Root） | 是 | 差（OS 更新即失效） | ❌ 不可行，且违反安全模型 |
| A2 | 借用现有插件（DFRTouch/HSTouch）给 HOUSI 提供 displayUUID | **0** | 否 | 否 | 否 | 无改动 | ❌ 现有插件没有这条逻辑 / 匹配不到 |
| B | HIDDriverKit dext（IOUserHIDEventService，设置 displayUUID） | **2** | **是**（DriverKit HID entitlement 审批） | 仅开发调试需要（正式签名分发不需要，但需用户批准扩展） | 开发期需重启 | 好（可卸载扩展） | ⚠️ 正统但昂贵；displayUUID 接受度未验证 |
| C | 虚拟 HID 设备（IOHIDUserDevice / CoreHID）+ displayUUID | **2** | **是**（`com.apple.developer.hid.virtual.device` 或 driverkit 变体） | 否（普通进程即可，只要签名/entitlement 通过） | 否 | 好（进程退出即消失） | ⚠️ 比 B 轻量；无公开成功案例，效果未验证 |
| D | Sidecar/BKTouch 私有注入 | **0**（第三方） | **是**（`com.apple.backboardd.touchStreams` 等 Apple 私签，无公开申请） | 否 | 否 | — | ❌ 仅 Apple 内部可用；可作机制参考 |
| E | 用户态事件注入（CGEvent / NSEvent / 辅助功能） | **5** | 否 | 否 | 否 | 好 | ✅ 现状可落地；但**非原生**（无 NSTouch/DirectTouch，仅鼠标/滚轮/手势语义） |
| F | 修改系统（关 SIP + 塞插件/kext/改 /System） | **1** | 否 | **是** | 是 | 差（不可回退、每次更新失效、安全风险） | ⛔ 不建议（本次约束下不可做） |

### 推荐优先级（按"投入产出比"排序）

1. **P0（立即可做）**：用户态注入方案（CGEvent + NSEvent 手势/CGEvent 触控模拟），配合 HOUSI 显示器 UUID 做坐标映射；
   这正是 HIDTouch / MacTouch / glasses666 项目的现实做法，也是 Topti 最终选择的路线。
   可叠加 **Accessibility/Input Monitoring** 权限，零系统改动、可回退。
2. **P1（中期、需预算）**：走 Apple entitlement 申请：
   - 先申请 **`com.apple.developer.hid.virtual.device`**（C 路线）：只需 App + 签名，无需关 SIP 分发，验证成本最低；
   - 同时申请 **DriverKit HID 组**（`com.apple.developer.driverkit` + `family.hid.eventservice` + `transport.hid` + 可选的
     `family.hid.virtual.device`），用于 B 路线；
   - 申请前需要 **付费 Apple Developer Program 账号**；开发/调试需要一台**允许关闭 SIP** 的测试机（本机不行）。
   - 拿到后第一件验证实验：在 digitizer service 上设置 `displayUUID = 30D985CD-...`，观察 WindowServer 的
     `ignoring digitizer event ... from unsupported service` 是否消失、`handle_digitizer_added` 是否成功。
3. **P2（长期、影响 Apple）**：按 glasses666 仓库的思路向 Apple 提交 feature request / Feedback：
   公开 external touch display 的关联 API，或把 external touchscreen 从 `SupportExternalTouchscreen`
   私有 feature flag 变成可用 API（该 flag 属 backboardd 私有特性，改它需要动系统配置，不在本方案内）。
4. **✗ 不做**：自研 HIDPlugin、kext、改 `/System` 或 `/Library/Apple`、关 SIP —— 全部违反本次只读约束，
   且可维护性/安全性极差。

---

## 7. 关键不确定性（需要后续实测才能定论）

| # | 问题 | 现状 | 验证方法 |
|---|---|---|---|
| 1 | 有了 virtual device entitlement 后，创建时携带的 `displayUUID`（device 属性）能否变成 event service 属性并被 WS 看到？ | 【推测】大概率不行（device 属性 ≠ service 属性）；Sidecar 是创建后再 `configure displayUUID` | 拿到 entitlement 后：`virtdev` 创建 + 设置，跑 WS 日志观察 |
| 2 | HIDDriverKit dext 在 `IOUserHIDEventService` 里 setProperty `displayUUID` 是否被 WS 接受？ | 【推测】机制上可行，无公开案例 | 需要 entitlement + 可关 SIP 的测试机；对比 WS 日志 |
| 3 | 插件是否只加载于 hidd、还是 hidd 与 WS 都加载？ | 【强推断】两者都可能；WS 日志证明插件代码确实运行在 WS 内 | 有 root 时可用 `fs_usage`/`dtrace` 观测 dlopen（本次不可用） |
| 4 | `com.apple.Multitouch:Plugin` 日志是否一定来自 `/System/Library/HIDPlugins/SessionFilters/MultitouchSessionFilter`？ | 【推测】子系统名 + 插件目录结构高度吻合，但未逐行证明 | root 下 `dtruss`/`sample` 或插件替换实验（不可做） |
| 5 | macOS 27 的 `SupportExternalTouchscreen` feature flag 是否符合预期（打开后仅影响 `NSScreen.touchCapabilities`） | 【公开资料】glasses666 仓库记录：flag 打开后 `touchCapabilities=0x1`，但仍无 displayUUID 关联 | 非本次范围（需改系统配置 + 重启） |

---

## 8. 证据与复现

### 8.1 本次生成的证据文件（`research/hid-channels/evidence/`）

| 文件 | 内容 |
|---|---|
| `01-plugin-search-paths.txt` | IOKit.framework 中 HIDPlugins 搜索路径 + 插件协议/匹配字符串 |
| `02-plugin-plists-signing.txt` | DFRTouch/HSTouch/DualSense 的 Matching 条件 + `anchor apple` 签名要求 |
| `03-virtual-hid-device-test.txt` | 普通用户创建虚拟 HID 设备返回 NULL + SDK 头文件原文 |
| `04-svcprobe.txt` | IOHIDEventSystem 只读服务枚举（182 个服务，HOUSI 在列，无 displayUUID 暴露） |
| `05-imgfind.txt` | 关键字符串 → 所在 Mach-O 镜像（IOKit / SkyLight / BackBoardHIDTouchEventProcessor / MultitouchSupport / UniversalHID 等） |
| `06-windowserver-codesign.txt` | WindowServer `library-validation` 标志 + 私有 HID entitlement；hidd entitlement；`/Library/Apple` restricted |
| `07-sidecar-bktouch.txt` | SidecarDisplayAgent 虚拟 digitizer 字符串/entitlement；replayd BKSTouchStream；BKTouch* 类名 |
| `08-hstouch-dfrtouch.txt` | HSTouchHIDService 的 displayUUID 逻辑字符串；DFRTouchServiceFilter 仅透传 |
| `09-ioreg-housi.txt` | HOUSI 事件服务属性摘录（DisplayIntegrated=Yes，无 displayUUID） |
| `10-systemext-driverkit-kext.txt` | SIP/Authenticated Root、systemextensionsctl 拒绝、SDK 缺 HIDDriverKit、DriverKit 三元组、kext 工具 |

### 8.2 本机 probe 源码（`research/hid-channels/probes/`）

| 源码 | 用途 | 构建命令 |
|---|---|---|
| `imgfind.c` | 遍历已加载镜像，定位关键字符串在哪个 framework/二进制 | `clang -O1 -o imgfind imgfind.c -framework CoreFoundation` |
| `dumpimg.c` | 打印某个已加载镜像里某个字符串前后上下文 | `clang -O1 -o dumpimg dumpimg.c -framework CoreFoundation` |
| `virtdev.c` | 尝试创建虚拟 13/4 digitizer（含 displayUUID 属性） | `clang -O1 -o virtdev virtdev.c -framework IOKit -framework CoreFoundation` |
| `svcprobe.c` | 只读枚举 IOHIDEventSystem 服务与属性 | `clang -O1 -o svcprobe svcprobe.c -framework IOKit -framework CoreFoundation` |

（全部 probe 只在进程内运行，不写系统位置；`virtdev` 创建失败即无任何副作用。）

### 8.3 复现关键命令

```bash
# 1) 确认 HOUSI 的 digitizer 服务与 WindowServer 的连接
ioreg -w0 -l -c AppleUserHIDEventService | grep -A3 -E 'VendorID|DisplayIntegrated|displayUUID'
ioreg -w0 -l | grep -ci displayUUID        # 0：displayUUID 不是内核属性

# 2) 确认 WindowServer 丢弃 digitizer
log show --last 5m --predicate 'process == "WindowServer"' | grep 'unsupported service'   # 需要权限；本会话被沙箱拒绝

# 3) HID 插件搜索路径（dyld 缓存 + IOKit.framework）
./probes/dumpimg IOKit.framework/Versions/A/IOKit "/System/Library/HIDPlugins" 30000 | grep -E 'HIDPlugins'

# 4) 插件签名 / WS library validation
codesign -d -r- /System/Library/HIDPlugins/ServiceFilters/DFRTouchServiceFilter.plugin
codesign -dvvv /System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/Resources/WindowServer | grep flags=

# 5) 虚拟 HID 设备实测（普通用户）
./probes/virtdev
```

---

## 9. 参考链接（公开资料）

1. glasses666，*macOS 27 external touchscreens — the `displayUUID` wall*：
   https://github.com/glasses666/macos27-external-touch
   （本机副本：`../evidence/g1_README.md` / `../evidence/g1_EVIDENCE.md`；关键记录：
   `SupportExternalTouchscreen` feature flag 打开后 `NSScreen.touchCapabilities=0x1`，但 WindowServer 仍因
   HID service 没有 `displayUUID` 而拒绝；用户态 `IOHIDServiceClientSetProperty` 不持久。）
2. HIDTouch：https://github.com/koshi545/HIDTouch
   （`IOHIDUserDevice` 虚拟 digitizer “blocked on an Apple-issued entitlement”。）
3. Topti（虚拟笔/tablet 方向，含 entitlement 与 DriverKit 的实证）：
   https://github.com/Yukaii/topti
   https://github.com/Yukaii/topti/blob/main/docs/decisions/0002-cgevent-tablet-output.md
4. MacTouch（用户态触屏驱动参考）：https://github.com/kashmeister/mactouch
5. Apple HIDDriverKit：https://developer.apple.com/documentation/hiddriverkit
6. Apple DriverKit entitlement 申请：
   https://developer.apple.com/documentation/driverkit/requesting-entitlements-for-driverkit-development
7. Apple 系统扩展调试（developer mode / SIP）：
   https://developer.apple.com/documentation/driverkit/debugging-and-testing-system-extensions
8. Apple 系统扩展安装（签名/entitlement 校验）：
   https://developer.apple.com/documentation/systemextensions/installing-system-extensions-and-drivers
9. Apple 虚拟 HID entitlement：
   https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.hid.virtual.device
   DriverKit 变体：https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.driverkit.family.hid.virtual.device
10. Apple CoreHID：https://developer.apple.com/documentation/corehid/hidvirtualdevice
11. Apple Kext 弃用说明：https://developer.apple.com/support/kernel-extensions/
12. Apple 芯片 kext 需 Reduced Security：https://support.apple.com/guide/mac-help/mchl768f7291/mac
13. Apple Developer Program（付费账号）：https://developer.apple.com/programs/

---

## 10. 一句话回复（给决策者）

> 在 SIP 开启、不改系统的前提下，外接触控屏**没有**任何"让 WindowServer 当原生触控"的可用第三方通道；
> 缺的是 HID event service 的 `displayUUID` 关联，这个键只由 Apple 私签的插件/驱动/虚拟设备在创建时写入。
> 短期只能做用户态注入（非原生）；要原生，必须走 Apple entitlement 申请（virtual HID device 或 HIDDriverKit），
> 或推动 Apple 公开 external touch display 的官方 API。
