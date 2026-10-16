#!/bin/sh
# mkvol.sh -- t_next's ufs volume (SCSI disk 6): the NeXT native host and
# a small root folder for the host filesystem, the kernel from the user's OPENSTEP CD (NEXTCD, default
# media/openstep/Openstep-4.2-Intel-User.iso), which go on the volume only.
#
#   sh tests/next/mkvol.sh outdir
#
# Out: outdir/next.img.  Without the CD nothing is made and t_next skips.
# NEXTROOT=dir (the CD tree tools/makenext.sh extracted, with dir.meta beside it) makes the whole
# tree the root instead of the few files: a larger volume and a slower t_next.
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
# a small root folder: the CD's first programs and what they load, and the sidecar
if [ -n "$NEXTROOT" ]; then
	[ -d "$NEXTROOT/NextApps" ] && [ -f "$NEXTROOT.meta" ] || { echo "[skip] no extracted tree $NEXTROOT"; exit 0; }
	cp "$NEXTROOT.meta" "$OUT/src/meta"
	{
		echo "f /native 755 0 3 native"
		echo "f /fs.meta 644 0 3 meta"
		printf 't\t/fs\t0\t3\t%s\n' "$NEXTROOT"
	} > "$OUT/manifest"
	python3 "$AUX/kernel/mac/diskroot/mkufs.py" -s "${NEXTROOT_MB:-640}" -t 723000000 -r "$OUT/src" -m /next \
		"$OUT/manifest" "$OUT/next.img"
	rm -rf "$OUT/src" "$OUT/nb"
	echo "[ok] $OUT/next.img (whole tree)"
	exit 0
fi
for f in usr/etc/init:usr/etc/init etc/mach_init:usr/etc/mach_init usr/lib/dyld:usr/lib/dyld \
    usr/shlib/libsys_s.B.shlib:usr/shlib/libsys_s.B.shlib bin/sh:bin/sh bin/echo:bin/echo \
    etc/rc.boot:private/etc/rc.boot etc/rc:private/etc/rc etc/ttys:private/etc/ttys; do
	python3 -I "$AUX/tools/nextcd.py" "$NEXTCD" cat "/${f#*:}" > "$OUT/src/$(echo "${f%%:*}" | tr / _)"
done
{
	echo "40755 0 0 1 ./etc"
	echo "40755 0 0 1 ./usr/etc"
	echo "100755 0 0 1 ./usr/etc/init"
	echo "100755 0 0 1 ./etc/mach_init"
	echo "40755 0 0 1 ./usr"
	echo "40755 0 0 1 ./usr/lib"
	echo "100755 0 0 1 ./usr/lib/dyld"
	echo "40755 0 0 1 ./usr/shlib"
	echo "100755 0 0 1 ./usr/shlib/libsys_s.B.shlib"
	echo "40755 0 0 1 ./bin"
	echo "100755 0 0 1 ./bin/sh"
	echo "100755 0 0 1 ./bin/echo"
	echo "100444 0 0 1 ./etc/rc.boot"
	echo "100444 0 0 1 ./etc/rc"
	echo "100644 0 0 1 ./etc/ttys"
} > "$OUT/src/meta"
{
	echo "f /native 755 0 3 native"
	echo "f /mach_kernel 444 0 3 mach_kernel"
	echo "d /fs 755 0 3"
	echo "d /fs/etc 755 0 3"
	echo "d /fs/usr 755 0 3"
	echo "d /fs/usr/etc 755 0 3"
	echo "d /fs/usr/lib 755 0 3"
	echo "d /fs/usr/shlib 755 0 3"
	echo "f /fs/usr/etc/init 755 0 3 usr_etc_init"
	echo "f /fs/etc/mach_init 755 0 3 etc_mach_init"
	echo "f /fs/usr/lib/dyld 755 0 3 usr_lib_dyld"
	echo "f /fs/usr/shlib/libsys_s.B.shlib 755 0 3 usr_shlib_libsys_s.B.shlib"
	echo "d /fs/bin 755 0 3"
	echo "f /fs/bin/sh 755 0 3 bin_sh"
	echo "f /fs/bin/echo 755 0 3 bin_echo"
	echo "f /fs/etc/rc.boot 444 0 3 etc_rc.boot"
	echo "f /fs/etc/rc 444 0 3 etc_rc"
	echo "f /fs/etc/ttys 644 0 3 etc_ttys"
	echo "f /fs/.meta 644 0 3 meta"
} > "$OUT/manifest"
python3 "$AUX/kernel/mac/diskroot/mkufs.py" -s 16 -t 723000000 -r "$OUT/src" -m /next \
	"$OUT/manifest" "$OUT/next.img"
rm -rf "$OUT/src" "$OUT/nb"
echo "[ok] $OUT/next.img"
