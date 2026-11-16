#!/bin/sh
set -e
SRC="$(cd "$(dirname "$0")" && pwd)"
install -d -m 755 /Applications/HOUSITouchHelper.app/Contents/MacOS
install -m 755 "$SRC/HOUSITouchHelper.app/Contents/MacOS/housitouch-setmode" /Applications/HOUSITouchHelper.app/Contents/MacOS/housitouch-setmode
install -m 644 "$SRC/HOUSITouchHelper.app/Contents/Info.plist" /Applications/HOUSITouchHelper.app/Contents/Info.plist
chown -R root:wheel /Applications/HOUSITouchHelper.app
codesign --force --sign - --identifier local.housitouch.helper /Applications/HOUSITouchHelper.app 2>/dev/null || true
install -m 644 "$SRC/local.housitouch.setmode.plist" /Library/LaunchAgents/local.housitouch.setmode.plist
chown root:wheel /Library/LaunchAgents/local.housitouch.setmode.plist
CONSOLE_UID=$(stat -f %u /dev/console)
CONSOLE_USER=$(stat -f %Su /dev/console)
launchctl bootout "gui/$CONSOLE_UID/local.housitouch.setmode" 2>/dev/null || true
launchctl asuser "$CONSOLE_UID" launchctl bootstrap "gui/$CONSOLE_UID" /Library/LaunchAgents/local.housitouch.setmode.plist 2>/dev/null || true
echo "HELPER INSTALLED (console user: $CONSOLE_USER uid=$CONSOLE_UID)"
