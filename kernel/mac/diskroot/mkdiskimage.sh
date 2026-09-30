#!/bin/sh
# mkdiskimage.sh -- build q800-unix-disk.img: the direct-boot disk (map,
# Apple_Driver, HFS with boot blocks and kernel, command line root=c0d0s1)
# plus Apple_UNIX_SVR2 root (s1) and swap (s2) partitions.  The root is
# rebuilt by mkdiskroot.sh with the same kernel as /stand/unix.
#
#   sh mkdiskimage.sh [kernel.elf [out.img]]
#
# Defaults: kernel/build/unix-mac.elf, out/q800-unix-disk.img.  Swap size:
# SWAPMB (default 32).  SPARE="MB ..." adds empty partitions (s4 up) for
# file-system tests.  The tape segments come from build/tape or $AMIX_TAPE.
set -e

D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
KERNEL=${1:-$AUX/kernel/build/unix-mac.elf}
OUT=${2:-$D/out/q800-unix-disk.img}
SWAPMB=${SWAPMB:-32}
B=$D/build
PY="nice -n 19 python3"

KERNEL=$KERNEL sh "$D/mkdiskroot.sh"
mkdir -p "$(dirname "$OUT")"

sh "$AUX/images/mkboot.sh" -c root=c0d0s1 "$KERNEL" "$OUT.new"
$PY "$D/addparts.py" "$OUT.new" "$B/root.img" "$SWAPMB" $SPARE

echo "== checks"
$PY "$AUX/images/auxsash.py" apm "$OUT.new"
$PY "$AUX/images/auxsash.py" apmcheck "$OUT.new"
"$AUX/kernel/mac/bootblk/build/mkbb" check "$OUT.new" "$KERNEL"
# the kernel's own partition scan, built for the host
cc -std=gnu89 -w -o "$B/apmtest" "$AUX/kernel/mac/scsi/test/apmtest.c" \
	"$AUX/kernel/mac/scsi/apm.c"
"$B/apmtest" "$OUT.new" | tee "$B/slices.txt"
set -- $(awk '$1 == "s1" { print $2, $3 }' "$B/slices.txt")
dd if="$OUT.new" of="$B/s1.img" bs=512 skip="$1" count="$2" status=none
cmp "$B/s1.img" "$B/root.img" || { echo "[FAIL] slice 1 is not the root image"; exit 1; }
rm -f "$B/s1.img"

mv "$OUT.new" "$OUT"
echo "== done: $OUT ($(du -k "$OUT" | cut -f1) KB on disk, $(wc -c < "$OUT") bytes)"
