#!/bin/bash
# build.sh — 一键构建 touchprobe (Swift/AppKit) 与 hidprobe (C/IOHIDManager)
#
# 仅依赖 Command Line Tools（clang / swiftc / macOS SDK），不需要 Xcode、不需要 DriverKit SDK。
# 用法:
#   ./build.sh          构建到 ./build/
#   CC=... SWIFTC=... ./build.sh   覆盖编译器
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"
BUILD="$HERE/build"
mkdir -p "$BUILD"

CC="${CC:-clang}"
SWIFTC="${SWIFTC:-swiftc}"

if ! command -v "$SWIFTC" >/dev/null 2>&1; then
    echo "错误: 找不到 swiftc。请先安装 Command Line Tools: xcode-select --install" >&2
    exit 1
fi
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "错误: 找不到 clang。" >&2
    exit 1
fi

echo "== 工具链 =="
echo "swiftc: $("$SWIFTC" --version 2>&1 | head -1)"
echo "clang : $("$CC" --version 2>&1 | head -1)"
echo "SDK   : $(xcrun --show-sdk-path 2>/dev/null || echo '(未知)')"
echo

echo "== [1/2] 编译 touchprobe (Swift + AppKit) =="
"$SWIFTC" -O -o "$BUILD/touchprobe" "$HERE/touchprobe.swift" -framework AppKit

echo "== [2/2] 编译 hidprobe (C + IOKit) =="
"$CC" -O2 -Wall -o "$BUILD/hidprobe" "$HERE/hidprobe.c" \
    -framework IOKit -framework CoreFoundation

echo
echo "== 构建完成 =="
ls -l "$BUILD/touchprobe" "$BUILD/hidprobe"
echo
echo "快速自检:"
"$BUILD/touchprobe" --help >/dev/null && echo "  touchprobe --help OK"
"$BUILD/hidprobe" --help >/dev/null && echo "  hidprobe  --help OK"
echo
echo "运行示例:"
echo "  $BUILD/touchprobe --duration 10"
echo "  $BUILD/hidprobe --list"
echo "  $BUILD/hidprobe --dump --seconds 15"
echo "  $BUILD/hidprobe --setmode            # 已是多点模式时只读验证，不写设备"
echo "  $BUILD/hidprobe --setmode --force    # 强制发送一次 SetReport"
echo "详细说明见 README.md"
