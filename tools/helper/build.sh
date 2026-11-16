#!/bin/sh
# 构建多点模式辅助 App（HOUSITouchHelper.app）。
#
# 重要：本 App 需要一次性「输入监控」授权（TCC）。ad-hoc 签名一旦重新编译/重签，
# 授权即失效，必须在「系统设置 → 隐私与安全性 → 输入监控」里重新授权。
# 也就是说：日常使用请勿反复重建；只有在换机器或调试时才重新构建。
#
# 依赖：Command Line Tools（clang + macOS SDK）。
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
CC="${CC:-clang}"
APP="$HERE/HOUSITouchHelper.app"

if [ ! -f "$APP/Contents/Info.plist" ]; then
    echo "错误：缺少 $APP/Contents/Info.plist" >&2
    exit 1
fi

mkdir -p "$APP/Contents/MacOS"

echo "== 编译 housitouch-setmode (C + IOHIDManager) =="
"$CC" -O2 -Wall -o "$APP/Contents/MacOS/housitouch-setmode" \
    "$HERE/housitouch-setmode.c" \
    -framework IOKit -framework CoreFoundation

echo "== ad-hoc 签名 =="
codesign --force --sign - --identifier local.housitouch.helper "$APP"

echo
echo "构建完成："
codesign -dv "$APP" 2>&1 | head -5
echo
echo "安装（需管理员密码，并会向当前登录用户注册 LaunchAgent）："
echo "  sudo $HERE/install.sh"
echo "首次安装后需授权：系统设置 → 隐私与安全性 → 输入监控 → 打开 HOUSITouchHelper"
