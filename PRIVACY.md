# 隐私与数据卫生说明

本仓库在建立与首次提交前，对工作目录做了**全量隐私扫描与脱敏**。本文件记录扫描口径、
处置结果，以及你自己复现该项目时该怎么处理。

> 结论先行：仓库内**不含**家目录路径、真实用户名、主机名、机器/显示器序列号、
> 邮件地址、密钥或令牌，也不含 6.1 GB 的 Apple 系统库提取缓存与编译产物。

---

## 1. 扫描口径

| 类别 | 正则 / 手段 | 结果 |
|---|---|---|
| 家目录绝对路径 | `/Users/[A-Za-z0-9._-]+` | 命中 8 个文件 → 全部脱敏 |
| 用户名 | `\bchen\b` | 命中日志与环境输出 → 替换为 `user` |
| 主机名 | `[A-Za-z0-9-]+\.local` | 命中仅存在于**未入库**的大日志 → 未入库即无泄露 |
| 机器序列号 | `C02[A-Z0-9]{5,}` | 命中 2 处（均在未入库的 `hidutil_dump.plist`） |
| MAC 地址 | `([0-9a-f]{2}:){5}[0-9a-f]{2}` | 无 |
| 邮件地址 | `[a-z0-9._%+-]+@[a-z0-9.-]+\.[a-z]{2,}` | 无 |
| 密钥 / 令牌 | `gho_`、`ghp_`、`sk-`、`BEGIN … PRIVATE KEY`、`api_key` | 无 |
| 口令 | `password` | 仅命中文档中的**说明性文字**（如“需要管理员密码”），无真实口令 |
| 内网 IP | `192.168.`、`10.`、`172.16-31.` | 无（命中的 `10.086` 等是时间戳数值，非 IP） |

扫描范围：全部 `.md/.txt/.c/.m/.swift/.sh/.plist/.json/.log/.html/.py`，排除
`tools/x86cache/`（系统库缓存，不入库）与构建目录。

---

## 2. 实际做了哪些脱敏

| 文件 | 改动 |
|---|---|
| `REPORT.md` | 项目绝对路径 → `$PROJECT` |
| `analysis/disasm/11-commands.log` | `ROOT=<家目录绝对路径>` → `ROOT=$(cd "$(dirname "$0")/../.." && pwd)` |
| `evidence/diag1.sh`、`evidence/postlogin_check.sh` | 写死的输出目录 → 脚本自解析 `$(dirname "$0")` |
| `evidence/diag1.log` | 家目录路径 → `$HOME` / `$PROJECT`；独立用户名 → `user`；工具私有目录 → `$HOME/.tooling` |
| `research/hid-channels/evidence/03-virtual-hid-device-test.txt` | 二进制路径 → `$PROJECT/...` |
| `tools/harness/samples/touchprobe-*.{txt,log}` | 日志内路径 → `$PROJECT/...`，文件属主列 → `user` |

**未入库（因此不存在泄露风险）**：`evidence/hidutil_dump.plist`（含本机序列号）、
`evidence/runtime-displayinfo.txt`（含内置显示屏 `DisplaySerialNumber`）、
所有超过 1 MB 的原始采集日志（含主机名 `*.local` 与用户名路径）。
排除清单逐条写在 [`.gitignore`](.gitignore) 中，重建方法见 [evidence/README.md](evidence/README.md)。

---

## 3. 刻意不入库的内容

1. **`tools/x86cache/`（约 6.1 GB）**——从 dyld 共享缓存提取的 Apple 系统库。
   属 Apple 版权产物，**不应分发**；且体积远超仓库合理上限。提取方法见
   [analysis/README.md](analysis/README.md)，任何人可自行在本机重建。
2. **`tools/dyldsrc/`**——Apple dyld 开源项目的本地副本，第三方代码，按需自行获取。
3. **编译产物**——含 ad-hoc 签名的 `.app` 二进制、模块缓存（`.swiftmodule` / `.pcm`）。
   体积大、与机器绑定，且可一键重建（`make build`）。
4. **超大原始日志**——重复采集的多份 6 小时/2 小时转储，仅保留代表性样本。

---

## 4. 你自己的复现注意事项

- 抓 `ioreg`、`hidutil`、`log show` 转储时，输出里常带**机器序列号**与 **USB 设备序列号**。
  入库前先过滤：
  ```bash
  grep -vE 'C02[A-Z0-9]{5,}|"USB Serial Number"'
  ```
- `log show` 的 `CFBundleExecutablePath` 字段会带完整家目录路径，务必替换。
- 本项目用到的 `displayUUID` 是**显示器 UUID**，属于技术标识、不含个人信息，
  因此文档中保留原值以保持可复现性；它无法反推设备或个人。

---

## 5. 仓库历史

首次提交即为已脱敏状态，**没有把敏感内容提交进历史后再删除**（`git filter-repo` 式的
“删了但还在历史里”在本仓库不会发生）。如需自行核查：

```bash
git log --all -p | grep -E '/Users/[^/]+|C02[A-Z0-9]{5,}' || echo "clean"
```
