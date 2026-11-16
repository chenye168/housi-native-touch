# HOUSI 外接触控屏 macOS 原生触控——构建入口
#
# 常用目标：
#   make build      编译全部工具（探针 / 开机 daemon / 多点模式辅助 App）
#   make harness    只编译 AppKit + HID 探针
#   make install    安装开机自动恢复（需 sudo）
#   make uninstall  完全卸载并回退系统
#   make clean      删除全部编译产物
#
# 依赖：macOS Command Line Tools（clang / swiftc / macOS SDK），不需要 Xcode。

.PHONY: all build harness daemon helper standalone install uninstall check clean

all: build

build: standalone harness daemon helper

standalone:
	@./tools/build-standalone.sh

harness:
	@./tools/harness/build.sh

daemon:
	@./tools/daemon/build.sh

helper:
	@./tools/helper/build.sh

install: build
	@echo ">>> 安装开机注入 daemon（root）"
	@sudo ./tools/daemon/install.sh
	@echo ">>> 安装多点模式辅助 App（root + 当前用户 LaunchAgent）"
	@sudo ./tools/helper/install.sh
	@echo
	@echo "一次性授权：系统设置 → 隐私与安全性 → 输入监控 → 打开 HOUSITouchHelper"
	@echo "（若列表中没有：点 “+”，选择 /Applications/HOUSITouchHelper.app）"

uninstall:
	@echo ">>> 卸载辅助 App"
	@sudo ./tools/helper/uninstall.sh
	@echo ">>> 卸载 daemon"
	@sudo ./tools/daemon/uninstall.sh
	@echo "已回退到系统默认（触控退回鼠标模拟模式），无需重启。"

check:
	@echo "daemon:   $(launchctl print system/local.housitouchd >/dev/null 2>&1 && echo running || echo not-loaded)"
	@echo "helper:   $(launchctl print gui/$$(stat -f %u /dev/console)/local.housitouch.setmode >/dev/null 2>&1 && echo loaded || echo not-loaded)"
	@echo "mode:     $(./tools/harness/build/hidprobe --setmode --dry-run 2>/dev/null | grep -i mode || echo 'hidprobe 未构建')"
	@echo "displayUUID:"
	@LC_ALL=C ioreg -r -c AppleUserHIDEventService -l -w0 2>/dev/null | grep -a -o '"displayUUID"="[^"]*"' || echo "  (未找到)"

clean:
	@rm -rf tools/harness/build tools/gesturewatch/build
	@rm -f tools/displayprobe tools/dscx tools/fastpath tools/ffcheck tools/hidmode_root \
	        tools/hidprop tools/hidprop2 tools/maindisplay tools/regmark tools/regset \
	        tools/svcmode tools/usbmode tools/usbprobe tools/usbreset
	@rm -f tools/daemon/housitouchd tools/daemon/housitouch-usbreset
	@rm -f tools/helper/housitouch-setmode
	@rm -rf tools/helper/HOUSITouchHelper.app/Contents/MacOS \
	        tools/helper/HOUSITouchHelper.app/Contents/_CodeSignature
	@rm -f research/hid-channels/probes/dumpimg research/hid-channels/probes/imgfind \
	        research/hid-channels/probes/svcprobe research/hid-channels/probes/virtdev
	@echo "clean 完成"
