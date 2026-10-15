#!/bin/sh
# mkvol.sh -- t_next's ufs volume (SCSI disk 6): the NeXT native host and
# a small root folder for the host filesystem, the kernel from the user's OPENSTEP CD (NEXTCD, default
# media/openstep/Openstep-4.2-Intel-User.iso), which go on the volume only.
#
#   sh tests/next/mkvol.sh outdir
#
# Out: outdir/next.img.  Without the CD nothing is made and t_next skips.
set -e
T=$(cd "$(dirname "$0")/.." && pwd)
AUX=$(cd "$T/.." && pwd)
OUT=$1
NEXTCD=${NEXTCD:-$AUX/media/openstep/Openstep-4.2-Intel-User.iso}
case $OUT in ""|/) echo "usage: mkvol.sh outdir" >&2; exit 2 ;; esac
rm -rf "$OUT"
if [ ! -f "$NEXTCD" ]; then
	echo "[skip] no $NEXTCD"
	exit 0
fi
mkdir -p "$OUT/src"
python3 -I "$AUX/tools/nextcd.py" "$NEXTCD" cat /mach_kernel > "$OUT/src/mach_kernel"
sh "$AUX/kernel/guest/next/build.sh" "$OUT/nb" > "$OUT/nb.log" 2>&1 || { tail "$OUT/nb.log"; exit 1; }
cp "$OUT/nb/native" "$OUT/src/"
# a one-file root folder and its sidecar
echo "root folder" > "$OUT/src/init"
{
	echo "40755 0 0 1 ./etc"
	echo "100755 0 0 1 ./etc/init"
} > "$OUT/src/meta"
{
	echo "f /native 755 0 3 native"
	echo "f /mach_kernel 444 0 3 mach_kernel"
	echo "d /fs 755 0 3"
	echo "d /fs/etc 755 0 3"
	echo "f /fs/etc/init 755 0 3 init"
	echo "f /fs/.meta 644 0 3 meta"
} > "$OUT/manifest"
python3 "$AUX/kernel/mac/diskroot/mkufs.py" -s 8 -t 723000000 -r "$OUT/src" -m /next \
	"$OUT/manifest" "$OUT/next.img"
rm -rf "$OUT/src" "$OUT/nb"
echo "[ok] $OUT/next.img"
