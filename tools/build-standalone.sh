#!/bin/bash
# 构建 tools/ 下的独立探针（源码 ↔ 二进制一一对应）。
#
# 这些探针都是只读诊断工具：枚举 IOKit/HID/USB 服务、打印报告描述符、
# 设置 digitizer 的输入模式、提取 dyld 共享缓存等。
# 全部不使用 sudo（少数需要 root 的探针见 README 说明），不修改系统文件。
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

CC="${CC:-clang}"
SWIFTC="${SWIFTC:-swiftc}"

for tool in dscx fastpath ffcheck hidmode_root hidprop hidprop2 maindisplay regmark regset svcmode; do
    echo "== $tool (C) =="
    "$CC" -O2 -Wall -o "$HERE/$tool" "$HERE/$tool.c" \
        -framework IOKit -framework CoreFoundation \
        -framework Foundation -framework CoreGraphics
done

for tool in usbmode usbprobe usbreset; do
    echo "== $tool (ObjC) =="
    "$CC" -O2 -Wall -o "$HERE/$tool" "$HERE/$tool.m" \
        -framework Foundation -framework IOKit
done

echo "== displayprobe (Swift) =="
"$SWIFTC" -O -o "$HERE/displayprobe" "$HERE/displayprobe.swift"

echo "== gesturewatch (Swift) =="
mkdir -p "$HERE/gesturewatch/build"
"$SWIFTC" -O -module-cache-path "$HERE/gesturewatch/build/modulecache" \
    -o "$HERE/gesturewatch/build/gesturewatch" "$HERE/gesturewatch/gesturewatch.swift"

echo
echo "构建完成："
ls -l "$HERE"/*.c "$HERE"/*.m "$HERE"/*.swift >/dev/null
ls -l "$HERE"| grep -E '^-rwx' || true
