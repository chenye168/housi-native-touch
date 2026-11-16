# 安全边界与负责任披露

本项目属于**互操作性**与**系统安全研究**：通过公开的 IOKit 属性注入，
让 macOS 正确接纳一台被系统误判为 `unsupported service` 的外接触控屏。
它**不是**越狱工具、不绕过代码签名、不修改系统二进制、不需要关闭 SIP。

---

## 1. 它会改动什么

安装后系统中新增/修改的文件只有：

| 路径 | 作用 | 卸载 |
|---|---|---|
| `/Library/LaunchDaemons/local.housitouchd.plist` | root 开机启动项 | 删除 |
| `/usr/local/libexec/housitouchd` | 注入 + 重枚举主程序 | 删除 |
| `/usr/local/libexec/housitouch-usbreset` | USB 重枚举子工具 | 删除 |
| `/Library/LaunchAgents/local.housitouch.setmode.plist` | 用户登录后跑一次模式恢复 | 删除 |
| `/Applications/HOUSITouchHelper.app` | 写 HID feature report | 删除 |
| `/var/db/housitouchd.lastreset` | 同一 boot 只重置一次的标记 | 删除 |
| `/var/log/housitouchd.log`、`/tmp/housitouch-setmode.log` | 日志 | 删除 |

`sudo make uninstall` 会逐项清除以上全部内容，**不触碰系统原有文件**。

---

## 2. 权限模型

- `housitouchd` 以 **root** 运行（需要 `IORegistryEntrySetCFProperty` 写 HID 事件服务属性、
  以及 `IOUSBHostDevice resetWithError`）。
- `HOUSITouchHelper` 以**登录用户**运行，需要一次性 **输入监控（Input Monitoring）** 授权
  （TCC），用于向 digitizer 写 feature report。没有该授权时只会打印失败的告警，不影响其他功能。
- `housitouchd` 不做网络访问，不下载任何内容，日志仅写本地文件。
- 源码全部可审计：`tools/daemon/housitouchd.c`（约 6.7 KB）、
  `tools/helper/housitouch-setmode.c`。

---

## 3. 已知风险

1. **依赖私有属性**：`displayUUID` 未公开；macOS 升级后行为可能改变，
   最坏情况是注入无效、触控退回鼠标模拟模式（不会损坏系统）。
2. **USB 重枚举**：同一 boot 最多重置一次（以 boottime 记录），避免设备反复掉线。
   重置会让触控屏短暂（约 70 ms）重新枚举。
3. **ad-hoc 签名**：辅助 App 使用 ad-hoc 签名，未做公证。重新签名会重置 TCC 授权。
4. **请勿用于**：绕过其他外设的安全策略、把不受信任的设备伪装成受信任设备。

---

## 4. 披露方式

如果你发现以下问题，欢迎**私下**报告（不要先开 public issue）：

- 注入逻辑导致系统不稳定 / 设备反复掉线；
- 权限提升路径（`housitouchd` 存在命令注入、路径劫持、TOCTOU 等）；
- 日志中意外写入敏感信息。

请在 issue tracker 中创建一个 **Security** 标签的私有 issue，或直接联系仓库维护者。
首次响应目标：7 天内确认，30 天内给出修复或缓解方案。

---

## 5. 免责声明

本项目按「现状」提供，不附带任何担保。对系统的修改由使用者自行承担风险；
在安装前请确保你理解上述改动，并能通过 `make uninstall` 回退。
