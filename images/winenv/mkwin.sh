#!/bin/sh
# mkwin.sh -- the Win16 environment's files: startwin, nothing else.
# It has its own KERNEL, USER and GDI and needs no Windows files; each
# user's C: (~/WIN16) is made on first run.
#
#   sh images/winenv/mkwin.sh outdir
#
# Out: outdir/win.cpio (usr/bin/startwin, owned by root) and
# outdir/guest.cpio (guest's ~/WIN16).  Without the AMIX toolchain both
# archives are empty.
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
OUT=$1
SYS=$AUX/toolchain/amix/m68k-cbm-sysv4/sysroot
O=$AUX/tests/build/obj
case $OUT in ""|/) echo "usage: mkwin.sh outdir" >&2; exit 2 ;; esac
rm -rf "$OUT"
mkdir -p "$OUT/stage" "$OUT/guest"
OUT=$(cd "$OUT" && pwd)
S=$OUT/stage
if [ -x "$AUX/toolchain/amix/bin/m68k-cbm-sysv4-gcc" ]; then
	if [ ! -f "$O/libextra.a" ]; then
		mkdir -p "$O"; rm -rf "$O/extra"; mkdir -p "$O/extra"
		(cd "$O/extra" && ar x "$SYS/usr/ccs/lib/libc.so" && rm -f libc.so.1 &&
		 ar rc ../libextra.a $(ar t "$SYS/usr/ccs/lib/libc.so" | grep -v '^libc.so.1$'))
		rm -rf "$O/extra"
	fi
	sh "$AUX/tests/win16/build.sh" "$OUT/b"
	mkdir -p "$S/usr/bin" "$OUT/guest/home/guest/WIN16/windows/system" "$OUT/guest/home/guest/WIN16/temp"
	cp "$OUT/b/startwin" "$S/usr/bin/"
	chmod 755 "$S/usr/bin/startwin"
	rm -rf "$OUT/b"
	echo "[ok] Win16: startwin"
else
	echo "[skip] Win16: no AMIX toolchain"
fi
chmod 755 "$S"
(cd "$S" && find . -depth -print | cpio -o -H newc -R 0:3 --quiet) > "$OUT/win.cpio"
(cd "$OUT/guest" && find home/guest/WIN16 -print 2> /dev/null |
	cpio -o -H newc -R 100:1 --quiet) > "$OUT/guest.cpio"
rm -rf "$S" "$OUT/guest"
