#!/bin/sh
# mkvol.sh -- t_v200's ufs volume (SCSI disk 4): the Voyage 200 native
# host and, when found, the user's Flash dump V200ROM (default
# voyage200v209.rom) from V200ZIP (default media/v200.zip), which goes
# on the volume only.
#
#   sh tests/v200/mkvol.sh outdir
#
# Out: outdir/v200.img.  Without the dump nothing is made and t_v200
# skips its calculator checks.
set -e
T=$(cd "$(dirname "$0")/.." && pwd)
AUX=$(cd "$T/.." && pwd)
OUT=$1
V200ZIP=${V200ZIP:-$AUX/media/v200.zip}
V200ROM=${V200ROM:-voyage200v209.rom}
case $OUT in ""|/) echo "usage: mkvol.sh outdir" >&2; exit 2 ;; esac
rm -rf "$OUT"
if [ ! -f "$V200ZIP" ]; then
	echo "[skip] no $V200ZIP"
	exit 0
fi
mkdir -p "$OUT/src"
unzip -p "$V200ZIP" "$V200ROM" > "$OUT/src/media"
sh "$AUX/kernel/guest/voyage200/build.sh" "$OUT/nb" > "$OUT/nb.log" 2>&1 || { tail "$OUT/nb.log"; exit 1; }
cp "$OUT/nb/native" "$OUT/src/"
{
	echo "f /native 755 0 3 native"
	echo "f /media 444 0 3 media"
} > "$OUT/manifest"
python3 "$AUX/kernel/mac/diskroot/mkufs.py" -s 8 -t 723000000 -r "$OUT/src" -m /v200 \
	"$OUT/manifest" "$OUT/v200.img"
rm -rf "$OUT/src" "$OUT/nb"
echo "[ok] $OUT/v200.img"
