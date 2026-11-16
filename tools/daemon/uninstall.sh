#!/bin/sh
# Remove the boot-time injector.
launchctl bootout system/local.housitouchd 2>/dev/null || true
rm -f /Library/LaunchDaemons/local.housitouchd.plist
rm -f /usr/local/libexec/housitouchd
rm -f /usr/local/libexec/housitouch-usbreset
rm -f /var/db/housitouchd.lastreset
rm -f /var/log/housitouchd.log
echo "UNINSTALLED"
