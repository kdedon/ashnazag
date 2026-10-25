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
# The Mandelbrot stationery was saved mid-recording: its window picture lacks
# the end opcode, so a tear-off replays heap junk off its segment and the tool
# faults (as on a Lisa).  The "mandel" check runs on a copy with the end added.
python3 -I - "$OUT/src/Lisa/{T271828}\$S1" <<'EOF'
import sys
p = sys.argv[1]
try:
    b = bytearray(open(p, 'rb').read())
except OSError:
    sys.exit(0)
i = b.find(bytes.fromhex('0cbf00000000016c02d0'))
if i >= 0 and i + 0xcc0 <= len(b) and b[i + 0xcbe] == 0x38 and b[i + 0xcbf] != 0xff:
    b[i:i + 2] = b'\x0c\xc0'
    b[i + 0xcbf] = 0xff
    open(p, 'wb').write(b)
EOF
[ ! -f "$LISAMEDIA/H.rom" ] || cp "$LISAMEDIA/H.rom" "$OUT/src/Lisa/lisa.rom"
printf 't\t/Lisa\t0\t3\t%s\n' "$OUT/src/Lisa" > "$OUT/manifest"
python3 "$AUX/kernel/mac/diskroot/mkufs.py" -s 40 -t 723000000 -r "$OUT/src" -m /lisa \
	"$OUT/manifest" "$OUT/lisa.img"
rm -rf "$OUT/src"
echo "[ok] $OUT/lisa.img"
