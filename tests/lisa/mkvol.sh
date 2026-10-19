#!/bin/sh
# mkvol.sh -- t_lisa's ufs volume (SCSI disk 6): the user's Lisa ProFile
# image (LISAMEDIA, default media/lisa/imported), which goes on the volume only.
#
#   sh tests/lisa/mkvol.sh outdir
#
# Out: outdir/lisa.img.  Without the image nothing is made and t_lisa's
# boot checks skip.
set -e
T=$(cd "$(dirname "$0")/.." && pwd)
AUX=$(cd "$T/.." && pwd)
OUT=$1
LISAMEDIA=${LISAMEDIA:-$AUX/media/lisa/imported}
case $OUT in ""|/) echo "usage: mkvol.sh outdir" >&2; exit 2 ;; esac
rm -rf "$OUT"
if [ ! -f "$LISAMEDIA/profile.dc42" ]; then
	echo "[skip] no $LISAMEDIA/profile.dc42"
	exit 0
fi
mkdir -p "$OUT/src"
cp "$LISAMEDIA/profile.dc42" "$OUT/src/"
[ ! -f "$LISAMEDIA/H.rom" ] || cp "$LISAMEDIA/H.rom" "$OUT/src/lisa.rom"
{
	echo "f /profile.dc42 444 0 3 profile.dc42"
	[ ! -f "$OUT/src/lisa.rom" ] || echo "f /lisa.rom 444 0 3 lisa.rom"
} > "$OUT/manifest"
python3 "$AUX/kernel/mac/diskroot/mkufs.py" -s 40 -t 723000000 -r "$OUT/src" -m /lisa \
	"$OUT/manifest" "$OUT/lisa.img"
rm -rf "$OUT/src"
echo "[ok] $OUT/lisa.img"
