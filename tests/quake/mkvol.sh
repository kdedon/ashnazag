#!/bin/sh
# mkvol.sh -- t_quake's ufs volume (SCSI disk 5): quake from gl/quake and,
# when found, the user's shareware id1/pak0.pak (QUAKEPAK, default
# media/quake/id1/pak0.pak), which goes on the volume only.
#
#   sh tests/quake/mkvol.sh outdir
#
# Out: outdir/quake.img.  Without both inputs nothing is made and
# t_quake skips.
set -e
T=$(cd "$(dirname "$0")/.." && pwd)
AUX=$(cd "$T/.." && pwd)
OUT=$1
QW=${QW:-$AUX/images/work/quake}
QUAKEPAK=${QUAKEPAK:-$AUX/media/quake/id1/pak0.pak}
case $OUT in ""|/) echo "usage: mkvol.sh outdir" >&2; exit 2 ;; esac
rm -rf "$OUT"
if [ ! -f "$QW/quake" ] || [ ! -f "$QUAKEPAK" ]; then
	echo "[skip] no $QW/quake (gl/quake/build.sh) or no $QUAKEPAK"
	exit 0
fi
mkdir -p "$OUT/src/id1"
cp "$QW/quake" "$OUT/src/"
cp "$QUAKEPAK" "$OUT/src/id1/pak0.pak"
{
	echo "f /quake 755 0 3 quake"
	echo "d /id1 777 0 3"
	echo "f /id1/pak0.pak 644 0 3 id1/pak0.pak"
} > "$OUT/manifest"
python3 "$AUX/kernel/mac/diskroot/mkufs.py" -s 32 -t 723000000 -r "$OUT/src" -m /quake \
	"$OUT/manifest" "$OUT/quake.img"
rm -rf "$OUT/src"
echo "[ok] $OUT/quake.img"
