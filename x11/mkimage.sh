#!/bin/sh
# mkimage.sh -- the direct-boot disk image with X: the disk root of
# kernel/mac/diskroot plus the X diff (tape segments 13 Xcore, 14 Xbasic,
# the server package), built in a scratch copy of the tree.
#
#   sh x11/mkimage.sh [kernel.elf [out.img]]
#
# Needs package.sh's $X11W/pkg.  Tape segments from $AMIX_TAPE or, for
# 02 03 10, the live diskroot's build/tape.
set -e

D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/.." && pwd)
X11W=${X11W:-$AUX/images/work/x11}
KERNEL=${1:-$AUX/kernel/build/unix-mac.elf}
OUT=${2:-$X11W/q800-x11.img}
T=$X11W/auxtree
R=$T/kernel/mac/diskroot

# the scripts find the repository at ../../.. of their directory
rm -rf "$T"
mkdir -p "$T/kernel/mac" "$R"
ln -s "$AUX/images" "$T/images"
ln -s "$AUX/toolchain" "$T/toolchain"
ln -s "$AUX/kernel/build" "$T/kernel/build"
for d in bootblk scsi; do ln -s "$AUX/kernel/mac/$d" "$T/kernel/mac/$d"; done
(cd "$AUX/kernel/mac/diskroot" &&
 cp -p mkdiskimage.sh mkdiskroot.sh root.manifest fstree.py mkufs.py ufscheck.py \
	mks5fs.py s5check.py addparts.py "$R/" && cp -rp etc "$R/")
mkdir -p "$R/build/tape"
for s in 02 03 10; do
	cp "${AMIX_TAPE:-$AUX/kernel/mac/diskroot/build/tape}/$s" "$R/build/tape/$s"
done
ln -s "$X11W/pkg" "$R/x11pkg"
(cd "$R" && patch -s -p4 < "$D/diskroot/x11-diskroot.diff")
# the Mac environment's files (images/macenv/mkmacimage.sh)
if [ -n "$MACPKG" ]; then
	ln -s "$MACPKG" "$R/macpkg"
	(cd "$R" && patch -s -p4 < "$AUX/images/macenv/mac-diskroot.diff")
fi
: "${AMIX_TAPE:?AMIX_TAPE: tape segments 13 and 14}"
AMIX_TAPE=$AMIX_TAPE ROOTMB=${ROOTMB:-96} nice -n 19 sh "$R/mkdiskimage.sh" "$KERNEL" "$OUT"
