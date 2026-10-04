#!/bin/sh
# Build images/q800-test.img: the A/UX 3.1 disk with our kernel as unix.coff
# next to A/UX Startup, and A/UX Startup set to autoboot it.
#
#	sh images/mkimage.sh [--small] [kernel.coff]
#
# Only the HFS partition changes.  The A/UX partitions, the driver and the
# partition map stay byte-identical to the source image (checked at the end).
#
# --small builds images/q800-test-small.img instead: the same HFS volume,
# compacted to a smaller size, right after the driver, with no A/UX
# partitions.  Catalog records keep their node IDs, so the Startup Items
# alias to A/UX Startup still resolves.
# Needs: unzip, python3, cc; hfsutils is built into toolchain/ on first use.
set -eu

IMAGES=$(cd "$(dirname "$0")" && pwd)
AUX=$(dirname "$IMAGES")
ZIP=$AUX/AUX_3_1_1GB_Use_In_Shoebill.zip
MEMBER=AUX_3_1_1GB.dsk
# the unzipped disk, when present, instead of the zip
DSK=$AUX/$MEMBER
SMALL=
if [ "${1:-}" = --small ]; then
	SMALL=$IMAGES/q800-test-small.img
	shift
fi
KERNEL=${1:-$AUX/kernel/build/unix-mac.coff}
OUT=$IMAGES/q800-test.img
TC=$AUX/toolchain
T=$TC/bin
HFSSRC=$TC/src/hfsutils-3.2.6
ELF2COFF=$AUX/kernel/mac/boot/build/elf2coff
WORK=$IMAGES/work
PY="nice -n 19 python3 $IMAGES/auxsash.py"
SHRINK="nice -n 19 python3 $IMAGES/hfsshrink.py"

# A/UX Startup settings (see README.md)
AUTOLAUNCH="launch -m -n unix.coff"
AUTORECOVERY="echo A/UX file system check skipped"
DELAY=10

die() { echo "mkimage: $*" >&2; exit 1; }

[ -f "$ZIP" ] || [ -f "$DSK" ] || die "missing $ZIP"
[ -f "$KERNEL" ] || die "missing $KERNEL (run sh kernel/build.sh)"

# hfsutils (hmount, hcopy, hattrib, hls, humount), hfsck and hfsfork
if [ ! -x "$T/hmount" ] || [ ! -x "$T/hfsck" ]; then
	mkdir -p "$TC/share/man/man1"
	[ -d "$HFSSRC" ] || tar -xzf "$TC/dl/hfsutils-3.2.6.tar.gz" -C "$TC/src"
	for p in "$TC"/hfsutils-patches/*.patch; do
		patch -d "$HFSSRC" -p1 -R -s -f --dry-run < "$p" >/dev/null 2>&1 ||
			patch -d "$HFSSRC" -p1 -s < "$p" || die "$p does not apply"
	done
	(cd "$HFSSRC" && { [ -f Makefile ] || nice -n 19 ./configure --prefix="$TC"; } &&
	 nice -n 19 make && make install_cli BINDEST="$T" MANDEST="$TC/share/man" &&
	 cd hfsck && nice -n 19 make && install -m 755 hfsck "$T/") >/dev/null
fi
if [ ! -x "$T/hfsfork" ] || [ "$IMAGES/hfsfork.c" -nt "$T/hfsfork" ]; then
	cc -O -I"$HFSSRC/libhfs" -o "$T/hfsfork" "$IMAGES/hfsfork.c" \
	    "$HFSSRC/libhfs/libhfs.a"
fi

rm -rf "$WORK"
mkdir "$WORK"
# hmount keeps its state in $HOME/.hcwd
HOME=$WORK
export HOME
trap 'rm -rf "$WORK"' EXIT
FULL=$OUT.new
[ -z "$SMALL" ] || FULL=$WORK/full.img

echo "== extracting $MEMBER (sparse)"
rm -f "$FULL"
if [ -f "$DSK" ]; then
	nice -n 19 dd if="$DSK" of="$FULL" bs=64k conv=sparse status=none
else
	nice -n 19 unzip -p "$ZIP" "$MEMBER" | dd of="$FULL" bs=64k conv=sparse status=none
fi

echo "== adding unix.coff ($(wc -c < "$KERNEL") bytes)"
"$T/hmount" "$FULL" 1 >/dev/null
"$T/hcopy" -r "$KERNEL" ":unix.coff"
"$T/hattrib" -t COFF -c SASH ":unix.coff"
"$T/hcopy" -m ":A/UX Startup" "$WORK/sash.bin"
"$T/humount"

echo "== setting A/UX Startup autoboot"
$PY macbin "$WORK/sash.bin" "$WORK/sash.rsrc"
$PY patch "$WORK/sash.rsrc" "$WORK/sash.new" \
    --var "autolaunch=$AUTOLAUNCH" --var "autorecovery=$AUTORECOVERY" \
    --recovery 2 --delay $DELAY
"$T/hfsfork" "$FULL" 1 ":A/UX Startup" r "$WORK/sash.new"

echo "== checking"
"$T/hfsck" -n "$FULL" 1 || die "hfsck reports errors"
"$T/hmount" "$FULL" 1 >/dev/null
"$T/hls" -l
"$T/hcopy" -r ":unix.coff" "$WORK/check.coff"
"$T/hcopy" -m ":A/UX Startup" "$WORK/check.bin"
"$T/hls" -l ":System Folder:Startup Items"
"$T/humount"
cmp "$KERNEL" "$WORK/check.coff" || die "unix.coff differs from $KERNEL"
echo "unix.coff data fork identical to $KERNEL"
if [ -x "$ELF2COFF" ]; then
	"$ELF2COFF" -c "$WORK/check.coff" || die "elf2coff -c rejects unix.coff"
fi
$PY macbin "$WORK/check.bin" "$WORK/check.rsrc"
cmp "$WORK/sash.new" "$WORK/check.rsrc" || die "A/UX Startup resource fork mismatch"
$PY show "$WORK/check.rsrc"
$PY apm "$FULL"
$PY hashes "$FULL" "$ZIP" "$MEMBER" || die "non-HFS areas differ from the source"

if [ -z "$SMALL" ]; then
	mv "$FULL" "$OUT"
	echo "== done: $OUT"
	exit 0
fi

# Small image: map, driver, compacted HFS volume
set -- $($PY apm "$FULL" | awk '$NF == "Apple_HFS" { print $3, $5 }')
HSTART=$1 HCOUNT=$2
echo "== compacting the HFS volume (blocks $HSTART-$((HSTART + HCOUNT - 1)))"
$SHRINK shrink "$FULL" $HSTART $HCOUNT "$WORK/hfs.vol"
rm -f "$SMALL.new"
$PY small "$FULL" "$WORK/hfs.vol" "$SMALL.new"
fallocate --dig-holes "$SMALL.new" 2>/dev/null || :

echo "== checking $SMALL"
$PY apm "$SMALL.new"
$PY apmcheck "$SMALL.new" "$FULL" || die "partition map check failed"
set -- $($PY apm "$SMALL.new" | awk '$NF == "Apple_HFS" { print $3, $5 }')
"$T/hfsck" -n "$SMALL.new" 1 || die "hfsck reports errors on the small image"
$SHRINK compare "$FULL" $HSTART $HCOUNT "$SMALL.new" $1 $2 ||
    die "compacted volume differs"
# the same files, forks, Finder info, dates and catalog IDs through libhfs
for img in "$FULL" "$SMALL.new"; do
	d=$WORK/mb.$(basename "$img")
	mkdir "$d"
	"$T/hmount" "$img" 1 >/dev/null
	"$T/hls" -ilaR > "$d.ls"
	$SHRINK paths "$d.ls" > "$d.paths"
	i=0
	while IFS= read -r p; do
		i=$((i + 1))
		"$T/hcopy" -m "$p" "$d/$i.bin" || die "cannot copy $p"
	done < "$d.paths"
	"$T/humount"
done
F=$WORK/mb.$(basename "$FULL")
S=$WORK/mb.$(basename "$SMALL.new")
cmp "$F.ls" "$S.ls" || die "catalog listings differ"
diff -r "$F" "$S" >/dev/null || die "MacBinary copies differ"
echo "$(wc -l < "$S.paths") files: listings (with catalog IDs) and MacBinary copies identical"
"$T/hmount" "$SMALL.new" 1
"$T/hcopy" -r ":unix.coff" "$WORK/small.coff"
"$T/hcopy" -m ":A/UX Startup" "$WORK/small.bin"
"$T/humount"
cmp "$KERNEL" "$WORK/small.coff" || die "unix.coff differs from $KERNEL"
echo "unix.coff data fork identical to $KERNEL"
if [ -x "$ELF2COFF" ]; then
	"$ELF2COFF" -c "$WORK/small.coff" || die "elf2coff -c rejects unix.coff"
fi
$PY macbin "$WORK/small.bin" "$WORK/small.rsrc"
cmp "$WORK/sash.new" "$WORK/small.rsrc" || die "A/UX Startup resource fork mismatch"
$PY show "$WORK/small.rsrc"

mv "$SMALL.new" "$SMALL"
echo "== done: $SMALL ($(wc -c < "$SMALL") bytes)"
