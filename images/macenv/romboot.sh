#!/bin/sh
# usage: romboot.sh IMAGE
# Boots IMAGE (snapshot) from the Quadra 800 ROM; fails unless the console
# reaches the login prompt.  QROM names the ROM; skipped when it is absent.
AUX=$(cd "$(dirname "$0")/../.." && pwd)
ROM=${QROM:-$AUX/Quadra 800.ROM}
[ -f "$ROM" ] || { echo "[skip] ROM boot check: no $ROM"; exit 0; }
T=$(mktemp -d "${TMPDIR:-/tmp}/romboot.XXXXXX")
sh "$AUX/images/qemu/run-mac.sh" --tmo 120 "$ROM" "$1" 32 "$T" wait:60 shot:login quit >/dev/null 2>&1
if grep -q 'login:' "$T/serial.log" 2>/dev/null; then
	rm -rf "$T"; echo "[ok] ROM boot reaches login"
else
	echo "[FAIL] $1 does not reach the login prompt from the Quadra ROM (log: $T/serial.log)"
	exit 1
fi
