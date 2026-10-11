#!/bin/sh
# mksys81.sh -- a Mac OS 8.1 System Folder for A/UX from the CD image:
# System, Finder, Appearance Extension, the base fonts and SimpleText as AppleDouble pairs
# (name, %name),
# the System's linked patches re-guarded for A/UX (auxguard.py), made
# the installed System rather than the CD's.  FULL=1: the CD's whole
# System Folder, with the folders a System Folder has and a stamp, as
# environment 8.1's template.
#
#   [FULL=1] sh images/macenv/mksys81.sh cd.iso outdir [auxroot]
#
# The CD is proprietary; the copy stays local.
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
B=$AUX/toolchain/bin
ISO=$1
OUT=$2
AR=$3
[ -f "$ISO" ] && [ -n "$OUT" ] || { echo "usage: mksys81.sh cd.iso outdir"; exit 2; }
case $OUT in /|"$HOME"|"$HOME"/) echo "mksys81.sh: refusing $OUT"; exit 2;; esac
case $ISO in -*) ISO=./$ISO;; esac
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
# hmount keeps its state in $HOME
HOME=$W
export HOME
"$B/hmount" "$ISO" > /dev/null
rm -rf "$OUT"
mkdir -p "$OUT/Fonts"
get() {	# hfs-path outdir
	"$B/hcopy" -m ":System Folder:$1" "$W/f.bin"
	rm -f "$2/${1##*:}" "$2/%${1##*:}"
	python3 "$AUX/tools/macbin2ad.py" -n "${1##*:}" "$W/f.bin" "$2"
}
tree() {	# hfs-dir outdir
	"$B/hls" -1aFN "$1" | while IFS= read -r n; do
		case $n in
		*:)	mkdir -p "$2/${n%:}"
			tree "$1:${n%:}" "$2/${n%:}" ;;
		*)	n=${n%\*}
			"$B/hcopy" -m "$1:$n" "$W/f.bin"
			python3 "$AUX/tools/macbin2ad.py" "$W/f.bin" "$2" > /dev/null ;;
		esac
	done
}
[ -z "$FULL" ] || tree ":System Folder" "$OUT"
get System "$OUT"
# patches that would run over A/UX's Memory Manager
python3 "$AUX/tools/auxguard.py" "$OUT/%System" > /dev/null
# the System as installed: the CD's 'boot' 3 demands 'xboo' and a locked
# startup volume, the installer's checks only when 'xboo' is there
"$B/hcopy" -m ":Full Install Pieces:Software Installers:System Software:Mac OS 8.1 Update:System Resources" "$W/f.bin"
python3 "$AUX/tools/macbin2ad.py" -n sr "$W/f.bin" "$W"
python3 "$AUX/tools/rsrcedit.py" "$OUT/%System" -d xboo -c "$W/%sr" boot 3
get Finder "$OUT"
# the Finder's AppearanceLib, named without a space for the test root
if [ -z "$FULL" ]; then
	mkdir -p "$OUT/Extensions"
	"$B/hcopy" -m ":System Folder:Extensions:Appearance Extension" "$W/f.bin"
	python3 "$AUX/tools/macbin2ad.py" -n Appearance "$W/f.bin" "$OUT/Extensions"
fi
for f in Chicago Charcoal Geneva Monaco Courier Times; do
	get "Fonts:$f" "$OUT/Fonts"
done
"$B/hcopy" -m ":Utilities:SimpleText" "$W/f.bin"
python3 "$AUX/tools/macbin2ad.py" -n SimpleText "$W/f.bin" "$OUT"
if [ -n "$FULL" ]; then
	mkdir -p "$OUT/Control Panels" "$OUT/Preferences"
	INSTALL=":Full Install Pieces:Software Installers:System Software"
	"$B/hcopy" -m "$INSTALL:Mac OS 8:Installation Tome" "$W/panels.bin"
	python3 "$AUX/tools/tome.py" extract "$W/panels.bin" "$OUT/Control Panels" Monitors
	"$B/hcopy" -m "$INSTALL:Mac OS 8.1 Update:Tome 1" "$W/panels.bin"
	python3 "$AUX/tools/tome.py" extract "$W/panels.bin" "$OUT/Control Panels" Appearance
	OT=":Full Install Pieces:Software Installers:System Software:Open Transport Software:Install 1:Open Transport Files"
	"$B/hcopy" -m "$OT:TCP/IP CDEV" "$W/f.bin"
	rm -f "$OUT/Control Panels/TCP:IP" "$OUT/Control Panels/%TCP:IP"
	python3 "$AUX/tools/macbin2ad.py" -n 'TCP/IP' "$W/f.bin" "$OUT/Control Panels"
	"$B/hcopy" -m "$OT:TCP/IP Preferences" "$W/f.bin"
	rm -f "$OUT/Preferences/TCP:IP Preferences" "$OUT/Preferences/%TCP:IP Preferences"
	python3 "$AUX/tools/macbin2ad.py" "$W/f.bin" "$OUT/Preferences"
fi
"$B/humount" > /dev/null
mkdir -p "$OUT/Apple Menu Items"
sh "$D/logout/build.sh" "$OUT/Apple Menu Items"
if [ -n "$AR" ] && [ -n "$FULL" ]; then
	f='Extensions/CommandShell VT102'
	for src in "$AR/mac/lib/SystemFiles/shared/$f" "$AR/mac/lib/SystemFiles/private/$f" "$AR/mac/sys/System Folder/$f" ""; do
		[ -f "$src" ] && break
	done
	[ -n "$src" ] || { echo "mksys81.sh: missing $f"; exit 1; }
	python3 "$AUX/tools/asingle2ad.py" "$src" "$OUT/$f"
fi
[ -n "$FULL" ] || exit 0
mkdir -p "$OUT/Preferences" "$OUT/Startup Items" "$OUT/Shutdown Items"
(cd "$OUT" && find . -type f ! -name .stamp -print0 | LC_ALL=C sort -z | xargs -0 sha256sum) |
	sha256sum | cut -c1-16 > "$OUT/.stamp"
python3 -c "import sys; sys.path.insert(0, sys.argv[1]); import macbin2ad
open(sys.argv[2], 'wb').write(macbin2ad.ad_header(b'TEXTttxt\x40\0', 0))" \
	"$AUX/tools" "$OUT/%.stamp"
