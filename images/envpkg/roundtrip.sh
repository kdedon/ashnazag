#!/bin/sh
# roundtrip.sh -- pkgadd and pkgrm environment packages on a booted disk
# image, then start the environment that stays from a guest login.
#
#   sh images/envpkg/roundtrip.sh image [outdir]
#
# The image (mkmacimage.sh's) is booted from the Quadra 800 ROM (ROM=) as
# check-image.sh does.  root, unattended (-n -a envpkg), installs envcore,
# tosenv and lisaenv over what is there, from PKGS (a directory of
# datastreams) or else from /var/spool/pkg; the checks and PKGS go on
# SCSI disk 1.  Then: pkgrm envcore must refuse (its dependents are in),
# pkgrm tosenv must refuse while /dev/tos is held open, then remove tosenv
# while guestcore stays.  The state goes to the printer port
# (outdir/serial-b.log); guest then runs startlisa (outdir/02-lisa.png).
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
IMG=$1
OUT=${2:-$AUX/images/work/roundtrip}
ROM=${ROM:-$AUX/Quadra 800.ROM}
[ -f "$IMG" ] || { echo "usage: roundtrip.sh image [outdir]"; exit 2; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
rm -rf "$OUT/pk"; mkdir "$OUT/pk"
SRC=/var/spool/pkg
if [ -n "$PKGS" ]; then
	ls "$PKGS"/*.pkg > /dev/null
	cp "$PKGS"/*.pkg "$OUT/pk/"
	SRC=/mnt
fi
# typed lines stay short: the checks run from the disk
cat > "$OUT/pk/rt.sh" <<EOF
A='-n -a envpkg'
st() {
	ls -l /dev/tos /usr/bin/starttos /etc/rc2.d/S05tos /usr/aux/lib/mod.d/guestcore /usr/aux/lib/mod.d/lisaguest
	pkginfo -l envcore tosenv lisaenv | grep VERSION
	modadmin -s
}
echo before; st
for p in envcore tosenv lisaenv; do pkgadd \$A -d $SRC/\$p-*.pkg all || echo pkgadd=1; done
echo added; st
pkgrm \$A envcore && echo core=0 || echo core=1
sleep 40 < /dev/tos &
sleep 3; modadmin -s
pkgrm \$A tosenv && echo busy=0 || echo busy=1
wait
pkgrm \$A tosenv && echo pkgrm=0 || echo pkgrm=1
echo removed; st; sync; echo done
EOF
(cd "$OUT/pk" && ls) | sed 's|.*|f /& 644 0 3 &|' > "$OUT/pk.manifest"
python3 "$AUX/kernel/mac/diskroot/mkufs.py" -s 24 -r "$OUT/pk" -m /mnt \
	"$OUT/pk.manifest" "$OUT/pk.img" > /dev/null
D1="-drive file=$OUT/pk.img,format=raw,if=none,id=hd1,snapshot=on -device scsi-hd,scsi-id=1,drive=hd1"
QEXTRA="-g 640x480x8 $D1" sh "$AUX/images/qemu/run-mac.sh" --tmo 900 "$ROM" "$IMG" 128 "$OUT" \
	wait:150 'type:root\n' wait:5 'type:\n' wait:10 \
	'type:test -b /dev/dsk/c1d0s0 || mknod /dev/dsk/c1d0s0 b 18 1\n' wait:3 \
	'type:mount -F ufs -r /dev/dsk/c1d0s0 /mnt\n' wait:5 \
	'type:sh /mnt/rt.sh > /dev/term/b 2>&1\n' wait:300 \
	'type:umount /mnt; exit\n' wait:15 \
	'type:guest\n' wait:5 'type:\n' wait:10 'type:startlisa\n' wait:90 shot:02-lisa quit \
	> "$OUT/run.log" 2>&1 || :
L=$OUT/serial-b.log
grep -q '^done' "$L" || { echo "[FAIL] no log on the printer port"; exit 1; }
! grep -q '^pkgadd=1' "$L" && grep -q '^added' "$L" || { echo "[FAIL] pkgadd failed"; exit 1; }
grep -q '^core=1' "$L" || { echo "[FAIL] pkgrm removed envcore while environments need it"; exit 1; }
grep -q '^busy=1' "$L" || { echo "[FAIL] pkgrm removed tosenv while its module was in use"; exit 1; }
grep -q '^pkgrm=0' "$L" || { echo "[FAIL] pkgrm tosenv failed"; exit 1; }
sed -n '/^removed/,/^done/p' "$L" > "$OUT/after.log"
grep -q 'starttos.*\(not found\|No such\)' "$OUT/after.log" ||
	{ echo "[FAIL] tosenv's files remain after pkgrm"; exit 1; }
grep 'guestcore\|lisaguest' "$OUT/after.log" | grep -qv 'not found\|No such' &&
	! grep -q 'guestcore.*\(not found\|No such\)' "$OUT/after.log" ||
	{ echo "[FAIL] removing tosenv took shared files"; exit 1; }
! grep -q 'Fault' "$L" || { echo "[FAIL] fault on the console log"; exit 1; }
echo "[ok] pkgadd, refusals and pkgrm; see $L and 02-lisa.png"
