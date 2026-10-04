#!/bin/sh
# mkdiskroot.sh -- build the disk root filesystem (ufs, 8 KB blocks, 1 KB
# fragments) from the AMIX 2.1 tape: core (02), bsd (03) and terminfo (10),
# adapted by root.manifest.
#
#   sh mkdiskroot.sh [tape-dir]
#
# tape-dir: the tape segments (SVR4 cpio); default $AMIX_TAPE.  Not needed
# once build/tape holds them.
# Out: build/root.img, checked, listed in build/root.lst.
# KERNEL (default kernel/build/unix-mac.elf) becomes /stand/unix, the
# namelist of ifconfig, netstat and crash.
# ROOTMB: size (default 64, a multiple of 4).  ROOTFS=s5 builds a 1 KB s5
# file system instead (ROOTINODES inodes, default 8192).
# Another port: DISKROOT_BUILD (work directory), DISKROOT_MANIFEST and
# DISKROOT_SRCDIR (its file sources), DISKROOT_SWAPPAGE=2 keeps swap's 2 KB
# pages.
set -e

D=$(cd "$(dirname "$0")" && pwd)
TAPE="${1:-$AMIX_TAPE}"
B="${DISKROOT_BUILD:-$D/build}"
KERNEL="${KERNEL:-$D/../../build/unix-mac.elf}"
ROOTMB="${ROOTMB:-64}"
ROOTINODES="${ROOTINODES:-8192}"
ROOTFS="${ROOTFS:-ufs}"
STAMP=723000000		# 1992-11-29, fixed for reproducible images
PY="nice -n 19 python3"

mkdir -p "$B/tape"
for s in 02 03 10; do
	[ -f "$B/tape/$s" ] && continue
	[ -f "$TAPE/$s" ] || { echo "[FAIL] no tape segment $s in '$TAPE'"; exit 1; }
	cp "$TAPE/$s" "$B/tape/$s"
done

[ -f "$KERNEL" ] || { echo "[FAIL] no kernel $KERNEL"; exit 1; }
cp "$KERNEL" "$B/unix"

# the tape's profile, shutdown and screendefs without the Amiga screens
x() { (cd "$B" && cpio -i --quiet --to-stdout "$1" < tape/02); }
x etc/profile | sed -e 's/TERM=amiga/TERM=vt100/' -e 's/^	if sioc$/	if false/' \
	> "$B/profile"
x usr/sbin/shutdown | sed -e 's,^if /usr/amiga/bin/sioc &&,if false \&\&,' > "$B/shutdown"
x etc/screendefs | grep '^#' > "$B/screendefs"
{ x etc/group | grep -v '^display:'; echo "display::25:"; } > "$B/group"
x etc/vfstab | sed -e "s,^\(/dev/dsk/c0d0s1[	 ].*[	 ]/[	 ]*\)s5,\1$ROOTFS," > "$B/vfstab"
grep -q "^/dev/dsk/c0d0s1.*$ROOTFS" "$B/vfstab" || { echo "[FAIL] tape vfstab changed"; exit 1; }
# flush the file systems before uadmin halts
for r in rc0 rc6; do
	x usr/sbin/$r | sed -e 's,^/sbin/umountall$,&\n/sbin/sync\n/usr/bin/sleep 5,' > "$B/$r"
	grep -q '^/usr/bin/sleep 5$' "$B/$r" || { echo "[FAIL] tape $r changed"; exit 1; }
done
x etc/motd | sed -e 's/Amiga Version 2\.1/Ash Nazag/' > "$B/motd"
grep -q 'Ash Nazag$' "$B/motd" || { echo "[FAIL] tape motd changed"; exit 1; }
# swap -l and -s count in 2 KB pages; this kernel's pages are 4 KB
x usr/sbin/swap > "$B/swap"
[ "$DISKROOT_SWAPPAGE" = 2 ] || $PY - "$B/swap" <<'EOF' || { echo "[FAIL] tape swap changed"; exit 1; }
import sys
d = bytearray(open(sys.argv[1], 'rb').read())
# asl.l #2 -> #3 (pages to blocks, -l); moveq #11 -> #12 (page shift, -s)
for off, old, new in ((0x1216, 0xe581, 0xe781), (0x121e, 0xe581, 0xe781),
		(0x1034, 0x780b, 0x780c), (0x1044, 0x780b, 0x780c), (0x1054, 0x780b, 0x780c)):
	if d[off] << 8 | d[off + 1] != old:
		sys.exit(1)
	d[off:off + 2] = bytes((new >> 8, new & 0xff))
open(sys.argv[1], 'wb').write(d)
EOF
grep -q 'TERM=vt100' "$B/profile" && grep -q '^	if false$' "$B/profile" &&
	grep -q '^if false &&' "$B/shutdown" || { echo "[FAIL] tape scripts changed"; exit 1; }

if [ "$ROOTFS" = ufs ]; then
	$PY "$D/mkufs.py" -s "$ROOTMB" -t "$STAMP" -r "${DISKROOT_SRCDIR:-$D}" "${DISKROOT_MANIFEST:-$D/root.manifest}" "$B/root.img.new"
	$PY "$D/ufscheck.py" "$B/root.img.new" -l > "$B/root.lst" || {
		tail -20 "$B/root.lst"; exit 1; }
else
	$PY "$D/mks5fs.py" -s $((ROOTMB * 1024)) -i "$ROOTINODES" -b 1024 -t "$STAMP" \
		-n root -p c0d0s1 -r "${DISKROOT_SRCDIR:-$D}" "${DISKROOT_MANIFEST:-$D/root.manifest}" "$B/root.img.new"
	$PY "$D/s5check.py" "$B/root.img.new" -l > "$B/root.lst" || {
		tail -20 "$B/root.lst"; exit 1; }
fi
mv "$B/root.img.new" "$B/root.img"
tail -2 "$B/root.lst"
sha256sum "$B/root.img"
