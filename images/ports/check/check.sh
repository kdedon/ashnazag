#!/bin/sh
# check.sh -- install a package release on a disk image and use each
# package.
#
#   sh images/ports/check/check.sh image reldir [outdir]
#
# reldir: mkports.sh's or mkrelease.sh's outdir.  Boots the image from the
# Quadra 800 ROM (ROM=), logs in as root and runs t.sh from SCSI disk 1,
# which holds reldir, ptyrun, the host's SSH key and the checks (check/*.t,
# sourced in order).  apkg installs every package of the catalog over
# HTTP from the host; the host logs in over SSH when a check prints
# sshwait.  A check prints NAME=0 when it passes.  Results go to the
# printer port (outdir/serial-b.log).
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
IMG=$1 REL=$2
[ -f "$IMG" ] && [ -f "$REL/catalog" ] || { echo "usage: check.sh image reldir [outdir]"; exit 2; }
REL=$(cd "$REL" && pwd)
OUT=${3:-$AUX/images/work/ports-check}
ROM=${ROM:-$AUX/Quadra 800.ROM}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
rm -rf "$OUT/pk" "$OUT/hostk" "$OUT/hostk.pub"
mkdir -p "$OUT/pk/rel"
cp -R "$REL/catalog" "$REL/catalog.md5" "$REL/pkgs" "$OUT/pk/rel/"
cp "$D"/*.t "$OUT/pk/"
ssh-keygen -q -t ed25519 -N '' -f "$OUT/hostk"
cp "$OUT/hostk.pub" "$OUT/pk/"
PORT=ptyrun
. "$AUX/images/ports/port.sh"
$CC $PCFLAGS -o "$OUT/pk/ptyrun" "$D/ptyrun.c"
free() { python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])'; }
HP=$(free) SP=$(free)
cat > "$OUT/pk/t.sh" <<EOI
H=10.0.2.2:$HP A="-n -a envpkg"
PATH=/opt/amix/bin:\$PATH:/usr/apkg/bin; export PATH
mkdir -p /tmp/apkgc
c=/tmp/rel.conf
printf 'repo=%s\npkgadd=/usr/sbin/pkgadd\npkgrm=/usr/sbin/pkgrm\ncachedir=/tmp/apkgc\nadmin=/var/sadm/install/admin/envpkg\n' \
	file:///mnt/rel > \$c
apkg -c \$c update > /dev/null 2>&1 && echo apkgfile=0 || echo apkgfile=1
sed "s|^repo=.*|repo=http://\$H/|" \$c > \$c.h
apkg -c \$c.h update > /dev/null 2>&1 && echo apkghttp=0 || echo apkghttp=1
for p in \`sed 1d /mnt/rel/catalog | cut -d'|' -f1\`; do
	pkginfo -q \$p || apkg -c \$c.h install \$p < /dev/null > /tmp/i.out 2>&1
	pkginfo -q \$p && echo install-\$p=0 ||
		{ echo install-\$p=1; tail -5 /tmp/i.out; }
done
T=/tmp/ports; rm -rf \$T; mkdir \$T; cd \$T
for t in /mnt/*.t; do echo "== \$t"; . \$t; done
cd /; rm -rf \$T; echo done
EOI
(cd "$OUT/pk" && find . ! -name . | sed 's|^\./||') | while read -r f; do
	if [ -d "$OUT/pk/$f" ]; then echo "d /$f 755 0 3"; else echo "f /$f 755 0 3 $f"; fi
done > "$OUT/pk.manifest"
python3 "$AUX/kernel/mac/diskroot/mkufs.py" -s 48 -r "$OUT/pk" -m /mnt \
	"$OUT/pk.manifest" "$OUT/pk.img" > /dev/null
(cd "$REL" && exec python3 -m http.server -b 127.0.0.1 "$HP") > "$OUT/http.log" 2>&1 &
HTTPD=$!
L=$OUT/serial-b.log
rm -f "$L" "$OUT/ssh.out"
D1="-drive file=$OUT/pk.img,format=raw,if=none,id=hd1,snapshot=on -device scsi-hd,scsi-id=1,drive=hd1"
QEXTRA="-g 640x480x8 $D1 -nic user,hostfwd=tcp:127.0.0.1:$SP-:22" setsid sh "$AUX/images/qemu/run-mac.sh" \
	--tmo 2400 "$ROM" "$IMG" 128 "$OUT" \
	wait:150 'type:root\n' wait:5 'type:\n' wait:10 \
	'type:test -b /dev/dsk/c1d0s0 || mknod /dev/dsk/c1d0s0 b 18 1\n' wait:3 \
	'type:mount -F ufs -r /dev/dsk/c1d0s0 /mnt\n' wait:5 \
	'type:sh /mnt/t.sh > /dev/term/b 2>&1\n' wait:2300 quit > "$OUT/run.log" 2>&1 &
QEMU=$!
# timeout runs QEMU in a process group of its own
stop() {
	kill -- -$QEMU $HTTPD $(ps -eo pid,comm,args | awk -v s="$OUT/qmp.sock" '$2 ~ /^(timeout|qemu)/ && index($0, s) { print $1 }') 2> /dev/null
	rm -f "$OUT/pk.img"
}
trap stop EXIT
n=0
until grep -q '^done' "$L" 2> /dev/null || ! kill -0 $QEMU 2> /dev/null || [ $n -ge 2300 ]; do
	if [ ! -f "$OUT/ssh.out" ] && grep -q '^sshwait' "$L" 2> /dev/null; then
		timeout 120 ssh -p "$SP" -i "$OUT/hostk" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
			-o IdentitiesOnly=yes -o IdentityAgent=none -o BatchMode=yes -o ConnectTimeout=100 \
			root@127.0.0.1 'touch /tmp/sshok' > "$OUT/ssh.out" 2>&1 || :
	fi
	sleep 5; n=$((n + 5))
done
grep -q '^done' "$L" || { echo "[FAIL] no end of the log on the printer port"; exit 1; }
bad=$(grep '^[a-z0-9-]*=[1-9]' "$L" || :)
[ -z "$bad" ] || { echo "[FAIL]" $bad; exit 1; }
! grep -qi 'fault\|illegal instruction\|core dumped' "$L" || { echo "[FAIL] fault in $L"; exit 1; }
echo "[ok] $(grep -c '^[a-z0-9-]*=0' "$L") checks; see $L"
