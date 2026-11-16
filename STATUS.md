# 当前状态（最终：全自动原生触控已实现；Dock/触控板手势为 macOS 限制）

**结论**：HOUSI 外接触控屏在 macOS 27.2.1 上已实现全自动原生触控：真实重启后**无需拔插、无需命令**，
Safari 滚动/捏合、菜单栏点击正常。详见 REPORT.md。

## 已知限制
- Dock 点击：WindowServer 已投递到 Dock（连接 0x5C903），但 Dock 的 direct-touch 对外接 digitizer 不响应；
  builtIn 分类/主屏切换/私有开关均已排除（REPORT.md §7）。
- 触控板系统手势（三/四指、边缘手势等 10 项）：属于 indirect gesture 管线，不适用于外接触控屏
  （GESTURES.md）。
- 窗口控制“触控放大”：依赖 hover/direct-touch 适配，本机只合成点击。

## 卸载/回退
sudo tools/helper/uninstall.sh && sudo tools/daemon/uninstall.sh
