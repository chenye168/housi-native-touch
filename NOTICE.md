# NOTICE

本仓库同时包含**自有代码**与**对第三方系统的研究记录**，两者权利归属不同。

## 1. 自有代码与文档

除下文第 2、3 节明确列出的内容外，本仓库的源代码、脚本、文档、验证页面与实测日志
均由本仓库贡献者创作，按 [MIT License](LICENSE) 授权。

## 2. Apple 系统二进制与提取物（**不在本仓库内分发**）

- `tools/x86cache/`（从 dyld 共享缓存提取的系统库）、`tools/dyldsrc/`（Apple dyld 开源项目副本）
  均已列入 `.gitignore`，**不随本仓库分发**。
- 仓库中的**反汇编片段**（`analysis/disasm/` 等）仅为说明 macOS 为何丢弃外接 digitizer 事件，
  属于互操作性（interoperability）与安全分析用途。片段的权利归 Apple Inc. 所有，不受本项目
  MIT 许可覆盖，也不构成对 Apple 二进制整体的再分发。
- 如需复现反汇编分析，请在你自己的 Apple 设备上、按 [analysis/README.md](analysis/README.md) 的
  步骤从本机系统中提取。

## 3. 第三方项目调研记录

`evidence/hidtouch_README.md`、`evidence/mactouch_README.md`、`evidence/g1_*`、`*_tree.json`
等文件是对既有开源项目（HIDTouch、MacTouch 等）的**调研摘录**，用于说明它们为何都无法绕开
`displayUUID` 这一前置条件。原文权利归各自作者，此处仅作引用与评述，不作再分发。

## 4. 商标

HOUSI 为其各自权利人所有的产品名称，本项目与 HOUSI 厂商无关联。
macOS / Apple / MacBook Pro 是 Apple Inc. 的商标。本项目为独立研究，未经 Apple 背书。

## 5. 免责声明

本项目按「现状」提供，不附带任何担保。对系统的修改由使用者自行承担风险；
安装前请阅读 [SECURITY.md](SECURITY.md)，并确认可以用 `sudo make uninstall` 回退。
