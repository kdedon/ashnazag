#!/bin/sh
# mkpkgimage.sh -- the disk image with network tools, A/UX programs and,
# optionally, apkg: root.manifest plus root.manifest.add, built by a
# scratch copy of the diskroot tooling (the live directory is not written).
#
#   sh mkpkgimage.sh [-q] [-s dir] [-p dir] work [kernel.elf [out.img]]
#
# -q	network for an emulated user-mode net (10.0.2.15, gateway 10.0.2.2); else
#	etc/network-config, which leaves the Ethernet unconfigured
# -s dir	put the files in dir in /var/spool/pkg
# -p dir	install the datastreams dir/APKGENG-*.pkg and dir/APKG-*.pkg
#	(pkginst.py); apkg uses the stock pkgadd and finds the repository
#	through the hosts file
# work	scratch directory; out.img defaults to work/q800-unix-disk.img
# The A/UX root comes from tests/aux/auxroot, segment 07's tools from
# tests/build/net07 (tests/net/getnet.sh); the tape segments from the live
# directory's build/tape.
set -e

P=$(cd "$(dirname "$0")" && pwd)
D=$(cd "$P/.." && pwd)
AUX=$(cd "$D/../../.." && pwd)
PY="nice -n 19 python3"
QEMU= STAGE= PKGS=
while [ $# -gt 0 ]; do
	case $1 in
	-q) QEMU=1; shift ;;
	-s) STAGE=$(cd "$2" && pwd); shift 2 ;;
	-p) PKGS=$(cd "$2" && pwd); shift 2 ;;
	*) break ;;
	esac
done
[ $# -ge 1 ] || { echo "usage: mkpkgimage.sh [-q] [-s dir] [-p dir] work [kernel.elf [out.img]]"; exit 2; }
mkdir -p "$1"
W=$(cd "$1" && pwd)
KERNEL=${2:-$AUX/kernel/build/unix-mac.elf}
OUT=${3:-$W/q800-unix-disk.img}
AUXROOT=$(cat "$AUX/tests/aux/auxroot")
NET07=$AUX/tests/build/net07
X=$W/x
for f in "$AUXROOT/shlib/libc1_s" "$NET07/usr/sbin/route" "$KERNEL"; do
	[ -f "$f" ] || { echo "[FAIL] missing $f"; exit 1; }
done

# the tooling's tree: diskroot copied, everything else linked
T=$W/tree
WD=$T/kernel/mac/diskroot
rm -rf "$T" "$X"
mkdir -p "$WD/build" "$X"
for l in images toolchain kernel/build kernel/mac/bootblk kernel/mac/scsi; do
	ln -s "$AUX/$l" "$T/$l"
done
(cd "$D" && cp -a addparts.py fstree.py mkdiskimage.sh mkdiskroot.sh mks5fs.py \
	mkufs.py root.manifest s5check.py ufscheck.py etc test "$WD/")
cp -a "$D/build/tape" "$WD/build/tape"
cp -a "$D/build/net07" "$WD/build/net07"

# guest modules against this kernel; auxreg
sh "$AUX/kernel/guest/build.sh" -m "$KERNEL" "$X/guest" > "$X/guest.log" 2>&1 ||
	{ tail "$X/guest.log"; exit 1; }
TC=$AUX/toolchain/amix
SYS=$TC/m68k-cbm-sysv4/sysroot
for s in "$AUX"/kernel/dlm/libmod/*.s; do
	"$TC/bin/m68k-cbm-sysv4-as" -o "$X/lm_$(basename "$s" .s).o" "$s"
done
nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -w -D__STDC__=0 -I"$AUX/kernel/dlm/include" \
	-c "$P/auxreg.c" -o "$X/auxreg.o"
"$TC/bin/m68k-cbm-sysv4-ld" -o "$X/auxreg" "$SYS/usr/ccs/lib/crt1.o" \
	"$SYS/usr/ccs/lib/crti.o" "$X/auxreg.o" "$X"/lm_*.o "$SYS/usr/lib/libc.so.1" \
	"$SYS/usr/ccs/lib/crtn.o"

# network configuration and hosts
if [ -n "$QEMU" ]; then
	cp "$P/etc/network-config.qemu" "$X/network-config"
else
	cp "$P/etc/network-config" "$X/network-config"
fi
(cd "$WD/build" && cpio -i --quiet --to-stdout etc/inet/hosts < tape/02) > "$X/hosts"

sed -e "s#@X@#$X#g" -e "s#@P@#$P#g" -e "s#@NET07@#$NET07#g" -e "s#@AUXROOT@#$AUXROOT#g" \
	"$P/root.manifest.add" > "$X/add.manifest"
if [ -n "$STAGE" ]; then
	for f in "$STAGE"/*; do
		echo "f /var/spool/pkg/$(basename "$f") 644 0 3 $f"
	done >> "$X/add.manifest"
fi
if [ -n "$PKGS" ]; then
	$PY "$P/pkginst.py" "$X/pkg" "$PKGS"/APKGENG-*.pkg "$PKGS"/APKG-*.pkg \
		>> "$X/add.manifest"
	# the stock pkgadd/pkgrm: APKGENG's pkginstall faults once it has
	# rewritten the contents database
	c=$X/pkg/root/etc/apkg.conf
	sed -e 's,^# pkgadd=/usr/sbin/pkgadd$,pkgadd=/usr/sbin/pkgadd,' \
		-e 's,^# pkgrm=/usr/sbin/pkgrm$,pkgrm=/usr/sbin/pkgrm,' "$c" > "$c.new"
	[ "$(grep -c '^pkg\(add\|rm\)=/usr/sbin/' "$c.new")" = 2 ] ||
		{ echo "[FAIL] apkg.conf changed"; exit 1; }
	mv "$c.new" "$c"
	# the repository is a name-based virtual host; apkg reads the hosts file
	ip=$(getent hosts pkg.amigaux.org | awk '{ print $1; exit }')
	[ -n "$ip" ] || { echo "[FAIL] cannot resolve pkg.amigaux.org"; exit 1; }
	printf '%s\tpkg.amigaux.org\n' "$ip" >> "$X/hosts"
fi
cat "$X/add.manifest" >> "$WD/root.manifest"

sh "$WD/mkdiskimage.sh" "$KERNEL" "$OUT"
