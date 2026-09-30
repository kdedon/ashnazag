#!/bin/sh
# mkimage.sh -- the disk root image with two 24 MB spare slices (s4, s5) and
# /s5disk run from inittab at boot.
#
#   sh tests/s5disk/mkimage.sh [--kernel-dir DIR]
#
# Builds in tests/s5disk/build/ from copies of DIR/mac/diskroot and
# DIR/mac/bootblk (default: kernel/), with DIR/build/unix-mac.elf.
# Needs the tape segments in kernel/mac/diskroot/build/tape (mkdiskroot.sh).
# Out: tests/s5disk/build/s5disk.img.
set -e
T=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$T/../.." && pwd)
KDIR=$AUX/kernel
[ "$1" = --kernel-dir ] && KDIR=$(cd "$2" && pwd)
W=$T/build/aux
rm -rf "$W"
mkdir -p "$W/kernel/mac"
ln -s "$AUX/images" "$W/images"
ln -s "$AUX/toolchain" "$W/toolchain"
ln -s "$KDIR/mac/scsi" "$W/kernel/mac/scsi"
cp -r "$KDIR/mac/bootblk" "$W/kernel/mac/bootblk"
D=$W/kernel/mac/diskroot
mkdir -p "$D/build/tape"
(cd "$KDIR/mac/diskroot" && cp -r *.py *.sh root.manifest etc "$D/")
for s in 02 03 10; do
	cp "$AUX/kernel/mac/diskroot/build/tape/$s" "$D/build/tape/$s"
done
cp "$T/s5disk" "$D/etc/s5disk"
echo 'f /s5disk	755 0 3 etc/s5disk' >> "$D/root.manifest"
sed -i 's,^si::sysinit:.*,&\nst::sysinit:/s5disk </dev/console >/dev/console 2>\&1,' \
	"$D/etc/inittab"
grep -q '^st::' "$D/etc/inittab"
SPARE="24 24" sh "$D/mkdiskimage.sh" "$KDIR/build/unix-mac.elf" "$T/build/s5disk.img"
