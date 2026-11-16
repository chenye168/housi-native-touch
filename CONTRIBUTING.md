# 贡献指南

感谢你愿意改进这个项目。这里的代码面向**系统级实验**，所以我们对证据与可复现性的要求
高于一般仓库。

---

## 1. 开发环境

```
macOS 27.x（已在 27.2.1 / 26B131 验证）
Command Line Tools（clang、swiftc、macOS SDK）—— 不需要 Xcode
可选：一台会被 macOS 判为 unsupported service 的外接 USB digitizer
```

```bash
git clone <repo-url>
cd <repo>
make build        # 编译全部工具，不安装
make harness      # 只编 AppKit / HID 探针
```

`make build` 不会修改系统。只有 `sudo make install` 才会写入 LaunchDaemon/LaunchAgent。

---

## 2. 提交前检查清单

- [ ] `make build` 通过，无新增 warning。
- [ ] 若改动涉及行为结论，在 `evidence/` 里补一份**最小可复现证据**（命令 + 输出片段），
      并在对应文档里引用它。
- [ ] **隐私自查**：不要提交家目录路径、真实用户名、主机名、设备/机器序列号、邮件地址、令牌。
      CI 会跑自动检查，但请不要依赖它。
- [ ] 不要提交 `tools/x86cache/`、`tools/dyldsrc/`、`build/`、二进制产物
      （`.gitignore` 已覆盖，`git add -f` 前请三思）。
- [ ] 更新 `CHANGELOG.md`（Unreleased 段落）。

---

## 3. 代码风格

- **C**：C11，`-Wall` 零警告；4 空格缩进；公开函数加简短注释说明 IOKit 语义。
- **ObjC**：优先用公开 API；必须用私有符号时集中在一处并注释来源（类名 + selector）。
- **Swift**：不引入第三方依赖；探针程序保持单文件可 `swiftc -O` 直编。
- **文档**：中文为主，技术名词保留英文原词；命令用围栏代码块；结论要能指向证据文件。
- **Shell**：`#!/bin/sh` 优先，`set -eu`；路径用 `$(cd "$(dirname "$0")" && pwd)`，不要写死家目录。

---

## 4. 关于「改结论」

本项目把「能/不能」写得比较硬（例如 Dock 点击是 macOS 内部限制）。
如果你有新的反汇编证据或实测结果能推翻它，欢迎提 PR——但请同时给：

1. 复现步骤；2. 原始日志/反汇编片段；3. 为什么旧结论不成立。
只改结论文字、不给证据的 PR 会被挂起。

---

## 5. 分支与提交信息

- 分支：`feat/<topic>`、`fix/<topic>`、`docs/<topic>`。
- 提交信息用祈使句，首行 ≤ 72 字符，例如：
  `inject: also handle digitizer hot-plug after boot`
- 涉及硬件常量（UUID、VID/PID）的改动，请在正文里写明测试机型与系统版本。

---

## 6. 行为准则

- 只在自己拥有的设备上测试；不要用本项目去绕过他人设备的安全策略。
- 讨论技术细节时给出可验证的引用（日志、反汇编、官方文档链接）。
- 不欢迎「关闭 SIP 就解决了」一类没有证据支撑的建议——本项目的前提就是不关 SIP。
