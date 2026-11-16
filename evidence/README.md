# evidence/ — 实测证据索引

本目录是**证物柜**：每条结论都要能在 `REPORT.md` / `GESTURES.md` / `analysis/` 里指向这里的
某个文件。为了仓库体积与隐私，这里只放**代表性样本**；超大原始转储留在本地，见下文。

> 采集环境：MacBook Pro 18,1（M1 Pro），macOS 27.2.1（26B131），SIP 开启；
> HOUSI 14" USB-C 触控屏（VID `0x32D7` / PID `0x0001`，Digitizer `0x0D/0x04`，10 指）。
> 时间均为 2026-11-16。

---

## 1. 最终验证（最重要的三份）

| 文件 | 证明什么 |
|---|---|
| `touchevents_final_reboot.txt` | 真实重启后 `digitizer attached … displayUUID: 30D985CD-…`，**`unsupported service` 计数 0** |
| `final_touch_evidence.txt` | 重启后 ioreg 上的 `displayUUID` 与 Input Mode=2 快照 |
| `touchevents_native_final.txt` | 原生触控生效后的 TouchEvents 样本（含 hit-test 与投递） |

---

## 2. 对照实验（按问题分组）

| 组 | 文件 | 结论 |
|---|---|---|
| 注入前 | `touchevents_6h.txt`†、`touchevents_now.txt`、`unsupported_hits.txt` | 无 `displayUUID` 时事件被判 `unsupported service` |
| 注入后 | `touchevents_after_relogin{,2,3}.txt`、`touchevents_postreboot{,2}.txt` | 注入生效后的投递路径 |
| 模式切换 | `touchevents_mode2.txt`、`multitouch_dump.log`、`multitouch_ws.log` | Input Mode=2 时多指报告与 WS 收到的 contact |
| 热插拔 / 重置 | `touchevents_replug2.txt`、`touchevents_usbreset.txt`、`touchevents_v3_reset.txt`、`ws_stream_replug.log` | 重枚举后重新注入 |
| WindowServer 重启 | `touchevents_wsrestart.txt` | 重启 WS 后的行为 |
| 手势管线 | `system-gesture.log`、`gesture_config_logs.txt` | indirect gesture 注册与触控板域配置 |

† `touchevents_6h.txt` 为超大原始转储，**未入库**（见第 4 节）。

---

## 3. 设备与服务转储

| 文件 | 内容 |
|---|---|
| `ioreg_hideventservice.txt` | HID 事件服务树（属性、UUID、digitizer 节点） |
| `cache_string_hits.txt` | 共享缓存中的关键字符串命中（`displayUUID`、`unsupported service` 等） |
| `dscx_x86.log` | 共享缓存提取过程的输出 |

---

## 4. 未入库的超大/敏感文件

为保持仓库可读并杜绝隐私泄露，下列文件**只在本地保留**：

| 文件 | 大小 | 未入库原因 |
|---|---|---|
| `all_gesture_logs.txt` | 25 MB | 多轮重复采集 |
| `backboard_2h.txt` | 8.9 MB | 原始转储 |
| `dock_proc_test.log` / `dock_test.log` / `dock_test2.log` | 4–6 MB | 原始转储，含完整家目录路径 |
| `digitizer_6h.txt` / `touchevents_6h.txt` / `usb2iic_6h.txt` | 2.6–4.8 MB | 6 小时原始转储 |
| `gesture-session.ndjson` / `gesture-session2.ndjson` | 1.6–1.9 MB | 含主机名 `*.local` |
| `hidutil_dump.plist` | 1.1 MB | **含本机机器序列号** |
| `touchevents_latest.txt` / `ws_stream_accepted.log` | 1–1.4 MB | 原始转储 |

完整排除清单见仓库根目录 [`.gitignore`](../.gitignore)。列表与原因同步记录在
[PRIVACY.md](../PRIVACY.md)。

---

## 5. 如何重新生成

所有证据都能用仓库内的工具与脚本重建（无需原作者的机器）：

```bash
# TouchEvents（WindowServer / BackBoard 触控投递）
log show --last 30m --info --debug --style compact \
    --predicate 'category == "TouchEvents"' > evidence/touchevents_now.txt

# digitizer / displayUUID 相关
log show --last 30m --info --debug --style compact \
    --predicate 'eventMessage CONTAINS[c] "digitizer" OR eventMessage CONTAINS[c] "displayUUID"'\
    > evidence/digitizer_now.txt

# 服务属性（注意：输出可能含序列号，入库前过滤）
ioreg -r -c AppleUserHIDEventService -l -w0 > evidence/ioreg_hideventservice.txt

# HID 模式与报告
./tools/harness/build/hidprobe --inspect > evidence/hidprobe-inspect.txt
./tools/harness/build/hidprobe --setmode --dry-run > evidence/hidprobe-mode.txt

# 端到端：AppKit 是否收到 direct-touch
./tools/harness/build/touchprobe --duration 30 --log evidence/touchprobe.log
```

现成的采集脚本：`diag1.sh`（全量只读诊断）、`postlogin_check.sh`（重新登录后抓 TouchEvents）。

> 入库前请务必脱敏：`grep -vE 'C02[A-Z0-9]{5,}|"USB Serial Number"' file`，
> 并替换掉家目录路径。见 [PRIVACY.md](../PRIVACY.md)。

---

## 6. 历史研究附录

`g1_README.md` / `g1_EVIDENCE.md` / `g1_tree.json`、`hidtouch_README.md`、
`mactouch_README.md`、`hidtouch_tree.json`、`mactouch_tree.json` 是研究阶段对既有开源项目
（HIDTouch、MacTouch 等）的调研记录，解释了它们为什么都无法绕开 `displayUUID` 这一关。
汇总结论见 [research/hid-channels/FEASIBILITY.md](../research/hid-channels/FEASIBILITY.md)。
