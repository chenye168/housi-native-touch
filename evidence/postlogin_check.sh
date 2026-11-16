#!/bin/bash
# Post-login verification: fetch WindowServer TouchEvents logs after re-login.
OUT="$(cd "$(dirname "$0")" && pwd)"
cd "$(dirname "$0")/.." || exit 1
{
echo "=== date ==="; date
echo "=== WindowServer process ==="
pgrep -x WindowServer | head -3
ps -o pid,user,etime,command -p "$(pgrep -x WindowServer | head -1)" 2>/dev/null | cut -c1-200
echo
echo "=== TouchEvents since 30m ago (info+debug) ==="
log show --last 30m --info --debug --style compact --predicate 'category == "TouchEvents"' 2>/dev/null > "$OUT/touchevents_after_relogin.txt"
wc -l "$OUT/touchevents_after_relogin.txt"
grep -a -iE 'digitizer|unsupported service|added for display|Services added' "$OUT/touchevents_after_relogin.txt" | tail -40
echo
echo "=== digitizer/displayUUID messages since 30m ==="
log show --last 30m --info --debug --style compact --predicate 'eventMessage CONTAINS[c] "digitizer" OR eventMessage CONTAINS[c] "displayUUID"' 2>/dev/null > "$OUT/digitizer_after_relogin.txt"
wc -l "$OUT/digitizer_after_relogin.txt"
grep -a -iE 'added for display|no usable displayUUID|malformed|unsupported|attached' "$OUT/digitizer_after_relogin.txt" | tail -30
echo
echo "=== BackBoard TouchEvents sample (tail) ==="
tail -15 "$OUT/touchevents_after_relogin.txt"
} 2>&1
