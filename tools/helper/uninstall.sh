#!/bin/sh
CONSOLE_UID=$(stat -f %u /dev/console)
launchctl bootout "gui/$CONSOLE_UID/local.housitouch.setmode" 2>/dev/null || true
rm -f /Library/LaunchAgents/local.housitouch.setmode.plist
rm -rf /Applications/HOUSITouchHelper.app
rm -f /tmp/housitouch-setmode.log /tmp/housitouch-setmode.out /tmp/housitouch-setmode.err
echo "HELPER UNINSTALLED"
