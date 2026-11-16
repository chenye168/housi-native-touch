#!/bin/bash
# Read-only diagnostic collection for macOS 27 external touch investigation.
# Runs outside the file sandbox because `log` and `ps` refuse to run sandboxed.
OUT="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$OUT"
exec > >(tee "$OUT/diag1.log") 2>&1
echo "=== date: $(date) ==="
sw_vers

echo
echo "=== processes (WindowServer/backboard/HID/driverkit) ==="
ps -axo pid,ppid,user,etime,comm 2>/dev/null | grep -iE 'windowserver|backboard|hid|driverkit|sky' | grep -v grep | head -40
echo "--- WindowServer full command ---"
WSPID=$(pgrep -x WindowServer 2>/dev/null | head -1)
echo "pid=$WSPID"
[ -n "$WSPID" ] && ps -p "$WSPID" -o pid,user,etime,rss,command 2>/dev/null | cut -c1-300

echo
echo "=== TouchEvents logs (info+debug, last 6h) ==="
log show --last 6h --info --debug --style compact --predicate 'category == "TouchEvents"' 2>/dev/null > "$OUT/touchevents_6h.txt"
wc -l "$OUT/touchevents_6h.txt"
sed -n '1,80p' "$OUT/touchevents_6h.txt"
echo "--- tail ---"
tail -120 "$OUT/touchevents_6h.txt"

echo
echo "=== digitizer/displayUUID/unsupported logs (last 6h) ==="
log show --last 6h --info --debug --style compact --predicate 'eventMessage CONTAINS[c] "digitizer" OR eventMessage CONTAINS[c] "displayUUID" OR eventMessage CONTAINS[c] "unsupported service"' 2>/dev/null > "$OUT/digitizer_6h.txt"
wc -l "$OUT/digitizer_6h.txt"
tail -150 "$OUT/digitizer_6h.txt"

echo
echo "=== USB2IIC logs (last 6h) ==="
log show --last 6h --info --debug --style compact --predicate 'eventMessage CONTAINS "USB2IIC"' 2>/dev/null > "$OUT/usb2iic_6h.txt"
wc -l "$OUT/usb2iic_6h.txt"
tail -100 "$OUT/usb2iic_6h.txt"

echo
echo "=== BackBoard subsystem (last 2h) ==="
log show --last 2h --info --debug --style compact --predicate 'subsystem == "com.apple.BackBoard"' 2>/dev/null > "$OUT/backboard_2h.txt"
wc -l "$OUT/backboard_2h.txt"
tail -80 "$OUT/backboard_2h.txt"

echo
echo "=== sudo test (do not prompt) ==="
if sudo -n true 2>/dev/null; then echo PASSWORDLESS_SUDO_OK; else echo NO_PASSWORDLESS_SUDO; fi

echo
echo "=== systemextensionsctl ==="
systemextensionsctl list 2>&1 | head -20

echo
echo "=== third-party extension/plugin directories ==="
ls -la /Library/HIDPlugins 2>&1
ls -la /Library/DriverExtensions 2>&1
ls -la /Library/Extensions 2>&1 | head -20

echo
echo "=== security posture ==="
csrutil status 2>&1
csrutil authenticated-root status 2>&1

echo
echo "=== displays (system_profiler) ==="
system_profiler SPDisplaysDataType 2>/dev/null

echo
echo "=== windowserver display uuid prefs ==="
plutil -p /Library/Preferences/com.apple.windowserver.displays.plist 2>/dev/null | head -80

echo
echo "=== feature flag storage / override files ==="
ls -la /Library/Preferences/com.apple*feature* 2>/dev/null
ls -la /private/var/db/com.apple*feature* 2>/dev/null
find /Library /private/var/db -maxdepth 3 -iname '*featureflag*' 2>/dev/null | head -20

echo
echo "=== DONE ==="
