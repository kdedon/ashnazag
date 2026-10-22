#!/bin/sh
# mkvol.sh -- t_lisa's ufs volume (SCSI disk 6): /Lisa, the files of the
# user's Lisa ProFile image (LISAMEDIA, default media/lisa/imported) as
# makelisa.sh installs them, and their ROM.
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
OUT=$(cd "$OUT" && pwd)
HOME=$OUT/src sh "$AUX/tools/makelisa.sh" office "$LISAMEDIA/profile.dc42"
[ ! -f "$LISAMEDIA/H.rom" ] || cp "$LISAMEDIA/H.rom" "$OUT/src/Lisa/lisa.rom"
printf 't\t/Lisa\t0\t3\t%s\n' "$OUT/src/Lisa" > "$OUT/manifest"
python3 "$AUX/kernel/mac/diskroot/mkufs.py" -s 40 -t 723000000 -r "$OUT/src" -m /lisa \
	"$OUT/manifest" "$OUT/lisa.img"
rm -rf "$OUT/src"
echo "[ok] $OUT/lisa.img"
