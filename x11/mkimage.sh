#!/bin/sh
# mkimage.sh -- the direct-boot disk image with X: the disk root of
# kernel/mac/diskroot plus the X diff (tape segments 13 Xcore, 14 Xbasic,
# the server package), built in a scratch copy of the tree.
#
#   sh x11/mkimage.sh [kernel.elf [out.img]]
#
# Builds the server, client libraries, clients and package as needed.  BOOTX=1 boots to xdm (/etc/default/x).
# XPKGS="manx xview" also installs those
# packages (x11/NAME/build.sh, after build.sh clibs) as pkgadd would.
# Tape segments from $AMIX_TAPE or, for 02 03 07 10, the live diskroot's
# build/tape.
set -e

D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/.." && pwd)
PLATFORM=${PLATFORM:-mac}
X11W=${X11W:-$AUX/images/work/x11-$PLATFORM}
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
	mks5fs.py s5check.py addparts.py "$R/" && cp -rp etc "$R/" &&
 mkdir "$R/pkg" && cp -p pkg/pkginst.py "$R/pkg/")
mkdir -p "$R/build/tape"
for s in 02 03 07 10; do
	cp "${AMIX_TAPE:-$AUX/kernel/mac/diskroot/build/tape}/$s" "$R/build/tape/$s"
done
# the X pieces it installs, built or refreshed here
[ -f "$X11W/src/xc/programs/Xserver/Xamix" ] || X11W=$X11W sh "$D/build.sh"
X11W=$X11W sh "$D/build.sh" clibs clients
X11W=$X11W sh "$D/package.sh"
ln -s "$X11W/pkg" "$R/x11pkg"
(cd "$R" && patch -s -p4 < "$D/diskroot/x11-diskroot.diff")
# the Mac environment's files (images/macenv/mkmacimage.sh)
if [ -n "$MACPKG" ]; then
	ln -s "$MACPKG" "$R/macpkg"
	(cd "$R" && patch -s -p4 < "$AUX/images/macenv/mac-diskroot.diff")
	[ -f "$MACPKG/rom" ] || sed -i '\|^f /etc/aux/rom	|d' "$R/root.manifest"
	[ -f "$MACPKG/a201.cpio" ] || sed -i '/a201\|mac6/d' "$R/root.manifest"
fi
pk=
for p in $XPKGS; do
	f=$X11W/$p/$(echo "$p" | tr a-z A-Z).pkg
	[ -f "$f" ] || X11W=$X11W sh "$D/$p/build.sh"
	pk="$pk $f"
done
[ -z "$pk" ] || python3 "$R/pkg/pkginst.py" "$X11W/xpkgs" $pk >> "$R/root.manifest"
if [ "$BOOTX" = 1 ]; then
	sed 's/^BOOT=.*/BOOT=xdm/' "$X11W/pkg/xdm/default-x" > "$R/default-x"
	echo "f /etc/default/x 644 0 3 default-x" >> "$R/root.manifest"
fi
: "${AMIX_TAPE:?AMIX_TAPE: tape segments 13 and 14}"
AMIX_TAPE=$AMIX_TAPE ROOTMB=${ROOTMB:-96} nice -n 19 sh "$R/mkdiskimage.sh" "$KERNEL" "$OUT"
