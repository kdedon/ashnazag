#!/bin/sh
# mksys81.sh -- a Mac OS 8.1 System Folder for A/UX from the CD image:
# System, Finder, the base fonts and SimpleText as AppleDouble pairs
# (name, %name),
# the System's linked patches re-guarded for A/UX (auxguard.py), made
# the installed System rather than the CD's.
#
#   sh images/macenv/mksys81.sh cd.iso outdir
#
# The CD is proprietary; the copy stays local.
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
B=$AUX/toolchain/bin
ISO=$1
OUT=$2
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
	python3 "$AUX/tools/macbin2ad.py" -n "${1##*:}" "$W/f.bin" "$2"
}
get System "$OUT"
# patches that would run over A/UX's Memory Manager
python3 "$AUX/tools/auxguard.py" "$OUT/%System" > /dev/null
# the System as installed: the CD's 'boot' 3 demands 'xboo' and a locked
# startup volume, the installer's checks only when 'xboo' is there
"$B/hcopy" -m ":Full Install Pieces:Software Installers:System Software:Mac OS 8.1 Update:System Resources" "$W/f.bin"
python3 "$AUX/tools/macbin2ad.py" -n sr "$W/f.bin" "$W"
python3 "$AUX/tools/rsrcedit.py" "$OUT/%System" -d xboo -c "$W/%sr" boot 3
get Finder "$OUT"
for f in Chicago Charcoal Geneva Monaco Courier Times; do
	get "Fonts:$f" "$OUT/Fonts"
done
"$B/hcopy" -m ":Utilities:SimpleText" "$W/f.bin"
python3 "$AUX/tools/macbin2ad.py" -n SimpleText "$W/f.bin" "$OUT"
"$B/humount" > /dev/null
mkdir -p "$OUT/Apple Menu Items"
sh "$D/logout/build.sh" "$OUT/Apple Menu Items"
