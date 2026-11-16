# tools/ — 工具索引

所有工具都只做两件事：**观测**系统状态，或**写设备/服务属性**（可控、可回退）。
不使用 sudo 的探针在文档里标注「无需 root」；需要 root 的只有安装类脚本与 `housitouchd`。

## 构建

```bash
make build                 # 全部
./tools/harness/build.sh   # AppKit + HID 探针
./tools/daemon/build.sh    # 开机 daemon
./tools/helper/build.sh    # 多点模式辅助 App
./tools/build-standalone.sh# 独立诊断探针
```

依赖 Command Line Tools 的 `clang` / `swiftc`，不需要 Xcode。

---

## 1. 部署工具（会被安装进系统）

| 目录 | 内容 | 说明 |
|---|---|---|
| `daemon/` | `housitouchd.c`、`housitouchd`、`housitouch-usbreset`、`install.sh`、`uninstall.sh`、`local.housitouchd.plist` | root LaunchDaemon：开机注入 `displayUUID`，竞争失败时 USB 重枚举 |
| `helper/` | `housitouch-setmode.c`、`HOUSITouchHelper.app/`、`install.sh`、`uninstall.sh`、`local.housitouch.setmode.plist` | 登录用户 App：写 feature report `0x13` 把 Input Mode 设为 2（多点） |

> ⚠️ `HOUSITouchHelper.app` 为 ad-hoc 签名，重建会重置「输入监控」TCC 授权。

---

## 2. 观测探针

| 工具 | 用途 | root |
|---|---|---|
| `harness/touchprobe` | AppKit 层：打印屏幕信息、监听 `NSEventTypeDirectTouch` / gesture | 否 |
| `harness/tsprobe` | AppKit 层：轻量触摸事件探针（验证 direct-touch 是否到达普通 App） | 否 |
| `harness/hidprobe` | HID 层：枚举、解析 report descriptor、dump input report、`--setmode` 读写 Input Mode | 否* |
| `gesturewatch/gesturewatch` | 观察系统手势事件流，核对 G01–G28 | 否（需辅助功能） |
| `displayprobe` | 打印显示器列表、UUID、主屏、旋转 | 否 |
| `maindisplay` | 临时切换主显示器并恢复（`--save` / `--housi` / `--restore`），用于 Dock 实验 | 否 |
| `usbprobe` | 反射 IOUSBHost 类/方法，枚举 USB 设备 | 否 |
| `svcprobe`† | 只读遍历 IOHIDEventSystem 服务表，确认 digitizer 没有 `displayUUID` | 否 |
| `imgfind`† | 在已加载 Mach-O 镜像里按字节搜索字符串（定位 `displayUUID` 等） | 否 |
| `dumpimg`† | 在共享缓存镜像中 dump 指定模式附近的字符串 | 否 |
| `virtdev`† | 测试普通用户进程能否拿到虚拟 HID digitizer | 否 |

† 属于 `research/hid-channels/probes/`，用于被否决路线的取证。

\* `hidprobe --setmode` 写 feature report 时需要「输入监控」授权（TCC）。

---

## 3. 写入 / 实验探针

| 工具 | 做什么 | 对应结论 |
|---|---|---|
| `hidprop` | 对 digitizer 服务尝试设置 `displayUUID`（多种类型）并回读 | 证明只有 CFString 被接纳 |
| `hidprop2` | 验证 `IOHIDServiceClientSetProperty` 设置的属性是否跨 client 持久 | 说明为何走 IORegistry 路径 |
| `regset` | 用 IORegistry 设置 `displayUUID` / `Built-In` 并回读 | 最终采用的注入方式 |
| `regmark` | 在 digitizer 服务上设置任意属性（`regmark <key> <value>`） | 调试用 |
| `fastpath` | 测试私有 `IOHIDServiceClientFastPathSetProperty` 及其相关 setter | 被否决路径 |
| `svcmode` | 用私有 `IOHIDServiceClientSetElementValue` 设 Input Mode | 被否决（改用 Feature Report） |
| `usbmode` | 用 USB control transfer 读写 feature report `0x13`（不需要 TCC） | 备用路径 |
| `usbreset` | 通过 IOUSBHost 重置设备（`--dry-run` 可只观察） | USB 重枚举兜底 |
| `hidmode_root` | 测试 root 进程能否绕过 Input Monitoring TCC 打开 digitizer | 权限研究 |
| `ffcheck` | 查询 `libsystem_featureflags` 的 domain/flag 状态 | 排除 `SupportExternalTouchscreen` |
| `dscx` | 驱动 `/usr/lib/dsc_extractor.bundle`，从 dyld 共享缓存提取镜像 | 反汇编前置 |
| `re/resolve_macho.py` | 解析提取后 dylib 的 selector 引用 / stub 目标 | 反汇编辅助 |

> 这些探针都会**打印第一次与第一次回读的差异**，便于确认写操作是否真的生效。

---

## 4. 反汇编脚本

`analysis/scripts/`（不在 tools/ 下，但同属工具链）：

| 脚本 | 用途 |
|---|---|
| `dscmap.py` | 把共享缓存里的文件偏移换算成虚拟地址 |
| `xrefs.py` | 查找字符串/符号的交叉引用 |
| `annotate.py` | 对 `llvm-objdump` 输出做注释（标注 objc_msgSend 选择器） |

用法见 [analysis/README.md](../analysis/README.md)。

---

## 5. 典型排查顺序

```bash
./tools/harness/build/hidprobe --list           # 设备在不在、描述符对不对
./tools/svcprobe                                 # 服务有没有 displayUUID
./tools/regset                                   # 手动注入一次并回读
log show --last 5m --info --debug --predicate 'category == "TouchEvents"' \
  | grep -aiE 'digitizer attached|unsupported service'
./tools/harness/build/hidprobe --setmode --dry-run   # 现在是几点模式？
```
