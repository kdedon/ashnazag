#!/bin/sh
# roundtrip.sh -- pkgrm and pkgadd an environment package on a booted
# disk image, then start the environment from a guest login.
#
#   sh images/envpkg/roundtrip.sh image [outdir]
#
# The image (mkmacimage.sh's) is booted from the Quadra 800 ROM (ROM=) as
# check-image.sh does.  root, unattended (-n -a envpkg), tries to remove
# tosenv, which smsqenv needs (pkgrm must refuse), removes smsqenv and
# tosenv, reinstalls them from /var/spool/pkg and logs the state between
# to the printer port (outdir/serial-b.log); guest then runs starttos
# (outdir/02-tos.png).
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
IMG=$1
OUT=${2:-$AUX/images/work/roundtrip}
ROM=${ROM:-$AUX/Quadra 800.ROM}
[ -f "$IMG" ] || { echo "usage: roundtrip.sh image [outdir]"; exit 2; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
A='-n -a envpkg'
S='ls -l /dev/tos /usr/bin/starttos /etc/rc2.d/S05tos; ls /tos; pkginfo tosenv; grep -c tosenv /var/sadm/install/contents; modadmin -s'
QEXTRA="-g 640x480x8" sh "$AUX/images/qemu/run-mac.sh" --tmo 900 "$ROM" "$IMG" 128 "$OUT" \
	wait:150 'type:root\n' wait:5 'type:\n' wait:10 \
	'type:exec > /dev/term/b 2>&1; echo before; modadmin -l tosguest\n' wait:5 \
	"type:$S\n" wait:10 \
	"type:pkgrm $A tosenv && echo refused=0 || echo refused=1; pkginfo tosenv\n" wait:60 \
	"type:pkgrm $A smsqenv tosenv && echo pkgrm=0 || echo pkgrm=1; echo removed\n" wait:60 \
	"type:$S\n" wait:10 \
	"type:pkgadd $A -d /var/spool/pkg/tosenv-1.0.pkg all && pkgadd $A -d /var/spool/pkg/smsqenv-1.0.pkg all && echo pkgadd=0 || echo pkgadd=1; echo added\n" wait:120 \
	"type:$S; sync; echo done\n" wait:10 \
	'type:exec > /dev/console 2>&1; exit\n' wait:15 \
	'type:guest\n' wait:5 'type:\n' wait:10 'type:starttos\n' wait:90 shot:02-tos quit \
	> "$OUT/run.log" 2>&1 || :
grep -q '^done' "$OUT/serial-b.log" || { echo "[FAIL] no log on the printer port"; exit 1; }
sed -n '/^removed/,/^added/p' "$OUT/serial-b.log" | grep -q 'not found\|No such' ||
	{ echo "[FAIL] tosenv's files remain after pkgrm"; exit 1; }
grep -q '^refused=1' "$OUT/serial-b.log" && ! grep -q 'Fault' "$OUT/serial-b.log" ||
	{ echo "[FAIL] pkgrm removed tosenv while smsqenv needs it"; exit 1; }
grep -q '^pkgrm=0' "$OUT/serial-b.log" && grep -q '^pkgadd=0' "$OUT/serial-b.log" ||
	{ echo "[FAIL] pkgrm or pkgadd failed"; exit 1; }
echo "[ok] pkgrm and pkgadd of tosenv; see $OUT/serial-b.log and 02-tos.png"
