# macOS 27 触控手势验证清单（2026 触控 MacBook Pro → 本机 HOUSI 外接屏实测）

日期：2026-11-16
目标机型：MacBook Pro（2026，PROLED XDR 显示屏，M5 Pro / M5 Max）—— Apple 首款触控 Mac
本机环境：MacBook Pro 18,1（M1 Pro，2021），macOS 27.2.1（26B131），
外接 HOUSI 14" USB-C 触控屏（1600×1066，已通过 displayUUID 注入被 WindowServer 作为原生触控屏接纳）

---

## 0. 先说清楚三件事

1. **官方清单来源**：Apple 支持文档《在 PROLED XDR 显示屏上使用触控手势》
   <https://support.apple.com/zh-cn/guide/macbook-pro-proled-xdr/wv2xzgrweunx/mac>
   以及配套设置页《更改触控显示屏设置》
   <https://support.apple.com/guide/mac-help/mchlp1226tds/mac>。
   下文 G01–G28 全部来自这两页，不是媒体推测。
2. **本机没有触控 MacBook Pro 的硬件**（无内建触控层、无灵动岛对应的触控逻辑、无“触控显示屏”设置面板）。
   能在本机验证的是：**同一套 macOS 27.2.1 触控/手势管线，对外接原生触控屏是否生效**。
   因此结果分三种：✅ 在 HOUSI 上生效 / ⚠️ 部分生效或行为不同 / ❌ 不生效（含硬件缺失）。
3. **“有用”判定标准**：不是“文档里写了”，而是“在 HOUSI 上真做这个动作，系统产生了文档描述的结果”。
   每项都给了可观察的通过判据。

---

## 0.1 怎么勾选

每张表最右列是 **可用**：`[ ]` = 未测，改成 `[x]` = 可用，`[~]` = 部分可用/行为不同，`[-]` = 不可用或不适用。
如果编辑器支持任务列表（Typora / Obsidian / VS Code），可直接用文末 **附录 A** 点击勾选，省得手改表格。

想要鼠标直接勾、并按 `G01 ✅ 可用` 格式一键复制反馈：用浏览器打开
[`checklist.html`](checklist.html)（同目录，结果自动存在本机）。

---

## 1. 手势总表（G01–G28）

### A. 选择、打开、导览（6）

| 编号 | 手势 | 官方动作 | 通过判据（HOUSI 上应看到） | 可用 |
|---|---|---|---|---|
| G01 | 轻点 | 选择项目/快速操作（接来电、开链接或通知、从程序坞启动 App） | 目标被选中或动作执行；单击等价 | [ ] |
| G02 | 轻点两下 | 从访达打开照片/文件/App；（宣传口径）双击窗口填满屏幕 | 文件/App 打开；或窗口放大 | [ ] |
| G03 | 按住后拖移 | 多选项目 | 出现多选（框选或连选） | [ ] |
| G04 | 按住项目 | 打开快捷键菜单（辅助点按/右键） | 弹出上下文菜单 | [ ] |
| G05 | 单指任意方向拖移 | 滚动/平移 | 内容跟随手指滚动 | [ ] |
| G06 | 快速上下轻扫 | 快速滚动 | 内容惯性快速滚动 | [ ] |

### B. 处理对象和内容（4）

> 这四项有现成的交互测试页：[`gesture-lab.html`](gesture-lab.html)（Safari 打开，自动判定）。

| 编号 | 手势 | 官方动作 | 通过判据 | 可用 |
|---|---|---|---|---|
| G07 | 双指开合 | 放大/缩小（照片等；系统级捏合缩放为可选开关） | 内容缩放 | [ ] |
| G08 | 双指旋转 | 旋转项目 | 项目跟随旋转 | [ ] |
| G09 | 按住拖到目标再抬手 | 拖放 | 项目落到目标位置 | [ ] |
| G10 | 拖项目角落手柄 | 调整大小/裁剪 | 尺寸变化/裁剪框变化 | [ ] |

### C. 管理窗口（6）

| 编号 | 手势 | 官方动作 | 通过判据 | 可用 |
|---|---|---|---|---|
| G11 | 拖标题栏 | 移动窗口 | 窗口跟随移动 | [ ] |
| G12 | 拖窗口边缘/角落 | 调整窗口大小 | 窗口尺寸变化 | [ ] |
| G13 | 快速向左/右甩窗口 | 平铺到半屏 | 窗口吸附为左/右半屏 | [ ] |
| G14 | 拖到屏幕左/右边缘高亮区松开 | 平铺窗口 | 出现高亮区并吸附 | [ ] |
| G15 | 轻点窗口控制 | 展开窗口控制（红黄绿） | 控制区出现/可点 | [ ] |
| G16 | 全屏时从菜单栏下扫或点左上角 | 显示窗口控制 | 全屏下出现控制条 | [ ] |

### D. 系统导览（12）

| 编号 | 手势 | 官方动作 | 通过判据 | 可用 |
|---|---|---|---|---|
| G17 | 三指上扫 | 打开调度中心（Mission Control） | 所有窗口缩略图铺开 | [ ] |
| G18 | 三指下扫 | App Exposé（当前 App 所有窗口） | 当前 App 窗口铺开 | [ ] |
| G19 | 台前调度开启时点屏幕左边缘 | 显示所有已打开 App | 左侧出现 App 列表 | [ ] |
| G20 | 右上角下扫 | 打开控制中心 | 控制中心面板出现 | [ ] |
| G21 | 三指左右扫 | 在空间之间切换 | 整屏横向切换空间 | [ ] |
| G22 | 拇指+三指外张 | 显示桌面 | 窗口全部让开露出桌面 | [ ] |
| G23 | 四指捏合 | 显示 App（聚焦的 App 视图） | App 列表/聚焦 App 视图出现 | [ ] |
| G24 | 单指从屏幕底部上扫 | 显示程序坞（隐藏时） | 程序坞升起 | [ ] |
| G25 | 单指从屏幕顶部边缘下扫 | 显示菜单栏（隐藏时） | 菜单栏出现 | [ ] |
| G26 | 单指从屏幕右侧边缘左扫 | 打开通知中心 | 通知中心面板出现 | [ ] |
| G27 | 单指从顶部边缘扫过灵动岛 | 打开聚焦 / Siri AI | 聚焦搜索框出现 | [ ] |
| G28 | 单指左右轻扫 | 在页面间轻扫（设置项“在页面间轻扫”） | 页面前进/后退（Safari 等） | [ ] |

> 注：G28 在《更改触控显示屏设置》中是可开关项；《使用触控手势》正文未单列，但属于官方设置内的手势。
> G23 官方设置页写作“在聚焦中显示 App（Show Apps in Spotlight）”，即 macOS 26 起取代 Launchpad 的 App 视图。

### E. 官方设置项（用于判定“被关掉”还是“不支持”）

系统设置 → **触控显示屏**（需向下滚动；仅触控 MacBook Pro 有）：
- 在页面间轻扫：开/关
- 在空间之间轻扫：三指 / 四指 / 关
- 调度中心：三指 / 四指 / 关
- App Exposé：三指 / 四指 / 关
- 通知中心：开/关
- 在聚焦中显示 App（拇指+三指捏合）：开/关
- 显示桌面（拇指+三指外张）：开/关

系统设置 → 辅助功能 → **触控** → **忽略触控手势**：彻底关闭触控输入。

> 本机是外接触控屏，`/System/Library/ExtensionKit/Extensions` 中无 Touch Display 设置扩展，
> 因此上面这些开关在本机大概率**看不到、也改不了**；这不等于手势不可用，默认值需要实测。

### F. macOS 27 其他触控相关项（非 G 表，但官方明确）

- **下拉刷新**：Safari、邮件、新闻、播客、日历中，单指从内容顶部向下拖 → 刷新（类似 iPhone）。
- **触控自动映射为点按**：未适配触控的旧 App 也能直接点按（Apple 新闻稿）。
- **长按程序坞图标放大**：iOS 风格长按反馈（macOS 27.2 beta 起）。
- **Sidecar 触控增强**：iPad 作为 Mac 副屏时可点按、单指滚动、系统手势（本机未验证）。

---

## 2. 在本机（HOUSI 外接屏）逐项实测的方法

前置：
1. HOUSI 已作为原生触控屏接纳（REPORT.md 的 displayUUID 方案），WindowServer 日志应无
   `unsupported service`。
2. 多点输入模式需为 2：
   `tools/harness/build/hidprobe --setmode`（回读 Mode=2；不是则加 `--force`）。
3. 把被测 App 窗口放到 HOUSI 屏（Safari / 备忘录 / 访达），主屏用于看本清单。
4. 后台已在记录：`evidence/gesture-session2.ndjson`（CGEvent 事件 + 窗口布局 + 前台 App/空间变化），
   失败时可用时间戳对照“系统到底收没收到”。

测试顺序建议（先易后难，避免互相干扰）：
- 第一轮：G01–G06（基础点按/滚动）—— 确认触控基本链路。
- 第二轮：G07–G10（捏合/旋转/拖放）—— 直接用 [`gesture-lab.html`](gesture-lab.html)：
  Safari 打开 → 窗口拖到 HOUSI → ⌃⌘F 全屏；四项会各自判定并显示事件来源
  （gesture / touch / wheel / pointer），最后点“复制 B 组结果”。
- 第三轮：G11–G16（窗口）—— 用备忘录窗口。
- 第四轮：G17–G28（系统手势）—— 每做一项先回到普通桌面，记录是否误触发别的（如切空间）。
- 最后：下拉刷新（Safari 顶部下拉）、G28 页面轻扫。

记录格式（直接按编号反馈即可）：

```
G01 ✅  单击链接正常
G17 ❌  三指上扫无反应（日志无窗口铺开）
G21 ⚠️  会切空间，但方向相反（屏幕旋转 180° 导致？）
```

---

## 3. 本机特有的风险点（预期会影响结果）

| 风险 | 影响的手势 | 说明 |
|---|---|---|
| 无灵动岛/刘海触控逻辑 | G27 | HOUSI 无灵动岛；顶部边缘下扫可能只出菜单栏或聚焦，需实测 |
| 外接屏非内建触控层 | G17–G28 | 系统手势可能绑定内建触控显示屏；外接 digitizer 是否进同一手势管线待测 |
| 屏幕旋转 180° | 全部方向类 | 手势方向/边缘判定可能按未旋转坐标计算，出现反向或错边 |
| 无“触控显示屏”设置面板 | E 组 | 无法确认 3/4 指配置，默认行为未知 |
| 触控为“直接触摸”而非触控板 | G13/G21 等 | 触控板的多指手势语义可能不复用于 direct touch |
| 输入模式回落 | 全部 | 重插/重启后 Mode 可能回 0，先 `--setmode` |

---

## 4. 证据与工具

- 手势事件/窗口布局记录：`tools/gesturewatch/gesturewatch.swift`（已编译 `build/gesturewatch`）
  - 运行：`tools/gesturewatch/build/gesturewatch --out evidence/gesture-session2.ndjson --duration 7200`
  - 记录：CGEvent type 22（滚动，含 phase）/29（触摸帧）、窗口布局变化、前台 App、空间切换。
  - 不加 `--nsevent` 不会触发辅助功能授权弹窗。
- 触控 HID 探针：`tools/harness/build/hidprobe`（模式/描述符/report）
- 既有原生触控结论：`REPORT.md`、`STATUS.md`

---

## 附录 A：可点击勾选清单（Typora / Obsidian / VS Code 直接点）

勾选 = 该项可用；部分可用或不可用的，在 `→` 后补一句现象即可。整段复制发我，或直接把本文件发我。

- [ ] G01 轻点 →
- [ ] G02 轻点两下 →
- [ ] G03 按住后拖移（多选）→
- [ ] G04 按住项目（右键菜单）→
- [ ] G05 单指拖移（滚动/平移）→
- [ ] G06 快速上下轻扫（快速滚动）→
- [ ] G07 双指开合（缩放）→
- [ ] G08 双指旋转 →
- [ ] G09 按住拖放 →
- [ ] G10 拖角落手柄（调整/裁剪）→
- [ ] G11 拖标题栏（移动窗口）→
- [ ] G12 拖边缘/角落（调整窗口）→
- [ ] G13 快速左右甩（半屏平铺）→
- [ ] G14 拖到左右边缘高亮区（平铺）→
- [ ] G15 轻点窗口控制 →
- [ ] G16 全屏下显示窗口控制 →
- [ ] G17 三指上扫（调度中心）→
- [ ] G18 三指下扫（App Exposé）→
- [ ] G19 台前调度点左边缘 →
- [ ] G20 右上角下扫（控制中心）→
- [ ] G21 三指左右扫（切换空间）→
- [ ] G22 拇指+三指外张（显示桌面）→
- [ ] G23 四指捏合（显示 App）→
- [ ] G24 底部上扫（程序坞）→
- [ ] G25 顶部边缘下扫（菜单栏）→
- [ ] G26 右边缘左扫（通知中心）→
- [ ] G27 顶部扫过灵动岛（聚焦/Siri）→
- [ ] G28 单指左右轻扫（页面间轻扫）→

---

## 5. 参考

1. Apple Support：在 PROLED XDR 显示屏上使用触控手势（中文）
   <https://support.apple.com/zh-cn/guide/macbook-pro-proled-xdr/wv2xzgrweunx/mac>
2. Apple Support：Use touch gestures on the PROLED XDR Display（英文）
   <https://support.apple.com/guide/macbook-pro-proled-xdr/wv2xzgrweunx/mac>
3. Apple Support：更改触控显示屏设置
   <https://support.apple.com/guide/mac-help/mchlp1226tds/mac>
4. Apple Newsroom（2026-10）：Apple 发布配备 PROLED XDR 显示屏和屏幕触控的 MacBook Pro
   <https://www.apple.com.cn/newsroom/2026/10/apple-unveils-groundbreaking-macbook-pro-with-proled-xdr-display/>
5. MacRumors（2026-10-05）：macOS 27.2 Adds Touch Gestures Ahead of MacBook Ultra
   <https://www.macrumors.com/2026/10/05/macos-27-2-touch-gestures/>
6. MacRumors（2026-10-29）：You Can Turn Off the New MacBook Pro's Touchscreen Functionality
   <https://www.macrumors.com/2026/10/29/macbook-pro-touch-input-can-be-turned-off/>
7. Apple：macOS 27 Golden Gate 功能页（下拉刷新等）
   <https://www.apple.com/os/macos/>
