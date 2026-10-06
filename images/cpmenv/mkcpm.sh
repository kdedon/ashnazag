#!/bin/sh
# mkcpm.sh -- the CP/M-68K environment's files: startcpm, DRI's CPM.SYS
# and the nine distribution disks' files, from which startcpm makes each
# user's A: (~/CPM/a.img) on first run.
#
#   sh images/cpmenv/mkcpm.sh outdir
#
# Out: outdir/cpm.cpio (usr/bin/startcpm, cpm/sys/CPM.SYS, cpm/dist/),
# owned by root.  CPMZIP names DRI's release zip (default:
# media/cpm68k/68kv1_3.zip); without it the archive is empty.
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
OUT=$1
CPMZIP=${CPMZIP:-$AUX/media/cpm68k/68kv1_3.zip}
export CPMZIP
SYS=$AUX/toolchain/amix/m68k-cbm-sysv4/sysroot
O=$AUX/tests/build/obj
case $OUT in ""|/) echo "usage: mkcpm.sh outdir" >&2; exit 2 ;; esac
rm -rf "$OUT"
mkdir -p "$OUT/stage"
OUT=$(cd "$OUT" && pwd)
S=$OUT/stage
if [ -f "$CPMZIP" ]; then
	# startcpm links libc's nonshared members, as the tests do
	if [ ! -f "$O/libextra.a" ]; then
		rm -rf "$O/extra"; mkdir -p "$O/extra"
		(cd "$O/extra" && ar x "$SYS/usr/ccs/lib/libc.so" && rm -f libc.so.1 &&
		 ar rc ../libextra.a $(ar t "$SYS/usr/ccs/lib/libc.so" | grep -v '^libc.so.1$'))
		rm -rf "$O/extra"
	fi
	sh "$AUX/tests/cpm/build.sh" "$OUT/b"
	B=$OUT/b/root/cpm
	mkdir -p "$S/usr/bin" "$S/cpm/sys" "$S/cpm/dist" "$OUT/zip"
	cp "$B/bin/startcpm" "$S/usr/bin/"
	cp "$B/sys/CPM.SYS" "$S/cpm/sys/"
	cp "$B"/dist/* "$S/cpm/dist/"
	# the other disks' files; a name already there (disk 1's) wins
	unzip -q -o "$CPMZIP" -d "$OUT/zip"
	for f in "$OUT"/zip/DISK[2-9]/*; do
		n=$(basename "$f")
		[ "$n" = CPM.SYS ] || [ -f "$S/cpm/dist/$n" ] || cp "$f" "$S/cpm/dist/"
	done
	rm -rf "$OUT/b" "$OUT/zip"
	chmod -R a+rX,go-w "$S"
	chmod 755 "$S/usr/bin/startcpm"
	chmod 444 "$S/cpm/sys/CPM.SYS" "$S"/cpm/dist/*
	echo "[ok] CP/M-68K: startcpm, $(ls "$S/cpm/dist" | wc -l | tr -d ' ') files for A:"
else
	echo "[skip] CP/M-68K: no $CPMZIP"
fi
# the archive carries its parents: they must not shut others out
chmod 755 "$S"
(cd "$S" && find . -depth -print | cpio -o -H newc -R 0:3 --quiet) > "$OUT/cpm.cpio"
rm -rf "$S"
