#!/bin/sh
# mksys76.sh -- a Mac OS 7.6.1 System Folder for A/UX from the CD image:
# System, Finder and the base fonts as AppleDouble pairs (name, %name),
# the System's linked patches re-guarded for A/UX (auxguard.py).
#
#   sh images/macenv/mksys76.sh cd.iso outdir
#
# The CD is proprietary; the copy stays local.
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
B=$AUX/toolchain/bin
ISO=$1
OUT=$2
[ -f "$ISO" ] && [ -n "$OUT" ] || { echo "usage: mksys76.sh cd.iso outdir"; exit 2; }
case $OUT in /|"$HOME"|"$HOME"/) echo "mksys76.sh: refusing $OUT"; exit 2;; esac
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
get Finder "$OUT"
for f in Chicago Geneva Monaco Courier Times; do
	get "Fonts:$f" "$OUT/Fonts"
done
"$B/humount" > /dev/null
