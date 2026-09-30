#!/bin/sh
# mkboot.sh -- build q800-unix.img, a disk the Quadra ROM boots straight
# into our kernel: the A/UX disk's partition map and Apple_Driver, then a
# small HFS volume holding our boot blocks and the kernel as file `unix'.
#
#	sh images/mkboot.sh [-c cmdline] [-m MB] [kernel.elf [out.img]]
#
# -c	kernel command line in the boot blocks (at most 127 bytes)
# -m	HFS volume size in MB (default: kernel + 1 MB, at least 4)
# Defaults: kernel/build/unix-mac.elf, images/q800-unix.img.  The map and
# driver come from images/q800-test-small.img (else q800-test.img).
# Needs python3, cc and hfsutils in toolchain/bin (mkimage.sh builds them).
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
if [ -f "$HERE/bootblk.s" ]; then
	AUX=$(cd "$HERE/../../.." && pwd)
else
	AUX=$(cd "$HERE/.." && pwd)
fi
IMAGES=$AUX/images
BBDIR=$AUX/kernel/mac/bootblk
T=$AUX/toolchain/bin
PY="nice -n 19 python3 $IMAGES/auxsash.py"

die() { echo "mkboot: $*" >&2; exit 1; }

CMD= MB=
while [ $# -gt 0 ]; do
	case $1 in
	-c) CMD=$2; shift 2 ;;
	-m) MB=$2; shift 2 ;;
	*) break ;;
	esac
done
KERNEL=${1:-$AUX/kernel/build/unix-mac.elf}
OUT=${2:-$IMAGES/q800-unix.img}
SRC=$IMAGES/q800-test-small.img
[ -f "$SRC" ] || SRC=$IMAGES/q800-test.img
[ -f "$SRC" ] || die "no A/UX disk image for the map and driver (run images/mkimage.sh --small)"
[ -f "$KERNEL" ] || die "missing $KERNEL (run sh kernel/build.sh)"
[ -x "$T/hformat" ] && [ -x "$T/hfsck" ] || die "hfsutils missing in $T (run images/mkimage.sh once)"

sh "$BBDIR/build.sh" >/dev/null
MKBB=$BBDIR/build/mkbb

WORK=$OUT.work
rm -rf "$WORK"
mkdir "$WORK"
trap 'rm -rf "$WORK"' EXIT
# hmount keeps its state in $HOME/.hcwd
HOME=$WORK
export HOME

echo "== flat kernel image"
"$MKBB" flat "$KERNEL" "$WORK/unix.bin"
len=$(wc -c < "$WORK/unix.bin")
[ -n "$MB" ] || MB=$(( (len + 2 * 1048576 - 1) / 1048576 ))
[ "$MB" -ge 4 ] || MB=4

echo "== HFS volume, $MB MB"
dd if=/dev/zero of="$WORK/hfs.vol" bs=1048576 count="$MB" status=none
"$T/hformat" -l "Unix" "$WORK/hfs.vol" 0 >/dev/null
"$T/hmount" "$WORK/hfs.vol" 0 >/dev/null
"$T/hcopy" -r "$WORK/unix.bin" ":unix"
"$T/humount"
"$MKBB" patch "$BBDIR/build/bootblk.bin" "$KERNEL" "$WORK/hfs.vol" "$CMD"
"$T/hfsck" -n "$WORK/hfs.vol" 0 || die "hfsck reports errors"

echo "== disk image"
rm -f "$OUT.new"
$PY small "$SRC" "$WORK/hfs.vol" "$OUT.new"
$PY apm "$OUT.new"
$PY apmcheck "$OUT.new" "$SRC" || die "partition map check failed"
"$T/hfsck" -n "$OUT.new" 1 || die "hfsck reports errors on the image"
"$T/hmount" "$OUT.new" 1 >/dev/null
"$T/hls" -l
"$T/hcopy" -r ":unix" "$WORK/check.bin"
"$T/humount"
cmp "$WORK/unix.bin" "$WORK/check.bin" || die "file unix differs from the flat kernel"
"$MKBB" check "$OUT.new" "$KERNEL" || die "boot-block check failed"

mv "$OUT.new" "$OUT"
echo "== done: $OUT ($(wc -c < "$OUT") bytes)"
