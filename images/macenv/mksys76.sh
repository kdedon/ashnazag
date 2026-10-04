#!/bin/sh
# mksys76.sh -- a Mac OS 7.6.1 System Folder for A/UX from the CD image:
# every item of the CD's System Folder and SimpleText as AppleDouble
# pairs (name, %name), the System's linked patches re-guarded for A/UX
# (auxguard.py), plus A/UX's items that still work on 7.6.1.  .stamp
# changes with the contents; startmac refreshes a user's copy when it
# differs.
#
#   sh images/macenv/mksys76.sh cd.iso outdir [auxroot]
#
# The CD and A/UX are proprietary; the copy stays local.
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
B=$AUX/toolchain/bin
ISO=$1
OUT=$2
AR=$3
[ -f "$ISO" ] && [ -n "$OUT" ] || { echo "usage: mksys76.sh cd.iso outdir [auxroot]"; exit 2; }
case $OUT in /|"$HOME"|"$HOME"/) echo "mksys76.sh: refusing $OUT"; exit 2;; esac
case $ISO in -*) ISO=./$ISO;; esac
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
# hmount keeps its state in $HOME
HOME=$W
export HOME
"$B/hmount" "$ISO" > /dev/null
rm -rf "$OUT"
mkdir -p "$OUT"
# HFS folder $1 into directory $2; names stay Mac Roman
get() {
	"$B/hls" -1aFN "$1" | while IFS= read -r n; do
		case $n in
		*:)	mkdir "$2/${n%:}"
			get "$1:${n%:}" "$2/${n%:}" ;;
		*)	n=${n%\*}
			"$B/hcopy" -m "$1:$n" "$W/f.bin"
			python3 "$AUX/tools/macbin2ad.py" "$W/f.bin" "$2" > /dev/null ;;
		esac
	done
}
get ":System Folder" "$OUT"
# patches that would run over A/UX's Memory Manager
python3 "$AUX/tools/auxguard.py" "$OUT/%System" > /dev/null
# Monitors' settings for the A/UX screen (slot $E, mode $80, colour);
# startmac fits the size to the display
python3 "$AUX/tools/rsrcedit.py" "$OUT/%System" -a scrn 0 \
	0001007a000ee0000000008077fea801ffffffff0000000001e002800000
# 7.6.1's control panels from the installer (Monitors & Sound is
# PowerPC only); they replace A/UX's 7.0 ones of the same name below
mkdir -p "$OUT/Control Panels"
"$B/hcopy" -m ":Software Installers:Mac OS 7.6:Installation Tome" "$W/f.bin"
python3 "$AUX/tools/tome.py" extract "$W/f.bin" "$OUT/Control Panels" \
	"Apple Menu Options" Color "Date & Time" "Desktop Patterns" \
	"Extensions Manager" "General Controls" Keyboard Labels Map Memory \
	Monitors Mouse Numbers Sound Text Views WindowShade
# no "not shut down properly" warning: fsck does the crash recovery
python3 "$AUX/tools/shutchk.py" "$OUT/Control Panels/%General Controls" > /dev/null
"$B/hcopy" -m ":Utilities:SimpleText" "$W/f.bin"
python3 "$AUX/tools/macbin2ad.py" -n SimpleText "$W/f.bin" "$OUT"
"$B/humount" > /dev/null
mkdir -p "$OUT/Preferences" "$OUT/Startup Items" "$OUT/Shutdown Items" \
	"$OUT/Spool Folder" "$OUT/PrintMonitor Documents"
# A/UX's AppleSingle files where 7.6.1 has none: the terminal and
# connection tools CommandShell needs, MacTCP for A/UX's TCP, Cache
# Switch and desk accessories.  Left out: A/UX's Finder, System, printing,
# AppleTalk and file sharing, which 7.6.1 replaces.
if [ -n "$AR" ]; then
	L="$AR/mac/lib/SystemFiles" F="$AR/mac/sys/System Folder"
	for f in "Extensions/CommandShell VT102" "Extensions/MacTCP Tool" \
		"Control Panels/Cache Switch" \
		"Control Panels/MacTCP" "Apple Menu Items/Alarm Clock" \
		"Apple Menu Items/Calculator" "Apple Menu Items/Chooser" \
		"Apple Menu Items/Key Caps" "Apple Menu Items/Puzzle" \
		"Apple Menu Items/Note Pad" "Apple Menu Items/Scrapbook" \
		"Apple Menu Items/Control Panels" "Apple Menu Items/CommandShell" \
		"Scrapbook File" "MacTCP DNR"; do
		[ ! -f "$OUT/$f" ] || continue
		for s in "$L/shared/$f" "$L/private/$f" "$F/$f" ""; do
			[ -f "$s" ] && break
		done
		[ -n "$s" ] || { echo "mksys76.sh: no A/UX $f"; exit 1; }
		cp "$s" "$OUT/$f"
	done
fi
(cd "$OUT" && find . -type f ! -name .stamp -print0 | LC_ALL=C sort -z | xargs -0 sha256sum) |
	sha256sum | cut -c1-16 > "$OUT/.stamp"
# invisible to the Finder: a header with the invisible flag
python3 -c "import sys; sys.path.insert(0, sys.argv[1]); import macbin2ad
open(sys.argv[2], 'wb').write(macbin2ad.ad_header(b'TEXTttxt\x40\0', 0))" \
	"$AUX/tools" "$OUT/%.stamp"
