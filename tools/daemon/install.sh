#!/bin/sh
# Install boot-time displayUUID injector for HOUSI digitizer (run as root).
set -e
SRC="$(cd "$(dirname "$0")" && pwd)"
install -d -m 755 /usr/local/libexec
install -m 755 "$SRC/housitouchd" /usr/local/libexec/housitouchd
install -m 755 "$SRC/housitouch-usbreset" /usr/local/libexec/housitouch-usbreset
install -m 644 "$SRC/local.housitouchd.plist" /Library/LaunchDaemons/local.housitouchd.plist
chown root:wheel /Library/LaunchDaemons/local.housitouchd.plist /usr/local/libexec/housitouchd /usr/local/libexec/housitouch-usbreset
launchctl bootout system/local.housitouchd 2>/dev/null || true
launchctl bootstrap system /Library/LaunchDaemons/local.housitouchd.plist
launchctl kickstart -k system/local.housitouchd 2>/dev/null || true
touch /var/log/housitouchd.log
chmod 644 /var/log/housitouchd.log
echo "INSTALLED"
launchctl print system/local.housitouchd 2>/dev/null | head -20
