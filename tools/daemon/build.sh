#!/bin/sh
# 构建开机注入 daemon 及其 USB 重枚举子工具。
# 依赖：Command Line Tools（clang + macOS SDK）。
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
CC="${CC:-clang}"

echo "== [1/2] housitouchd (C + IOKit) =="
"$CC" -O2 -Wall -o "$HERE/housitouchd" "$HERE/housitouchd.c" \
    -framework IOKit -framework CoreFoundation -framework SystemConfiguration

echo "== [2/2] housitouch-usbreset (ObjC + IOKit) =="
"$CC" -O2 -Wall -o "$HERE/housitouch-usbreset" "$HERE/../usbreset.m" \
    -framework Foundation -framework IOKit

echo
echo "构建完成："
ls -l "$HERE/housitouchd" "$HERE/housitouch-usbreset"
echo
echo "安装（需管理员密码）："
echo "  sudo $HERE/install.sh"
