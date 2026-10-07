#!/bin/sh
# mkcpm.sh -- the CP/M-68K environment's files: startcpm, DRI's CPM.SYS
# and the nine distribution disks' files, from which startcpm makes each
# user's A: (~/CPM/A) on first run.
#
#   sh images/cpmenv/mkcpm.sh outdir
#
# Out: outdir/cpm.cpio (usr/bin/startcpm, cpm/sys/CPM.SYS, cpm/dist/),
# owned by root, and outdir/guest.cpio (guest's ~/CPM/A, as startcpm
# makes it).  CPMZIP names DRI's release zip (default:
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
	# A: links to each file, X.68K to X.REL, and EXIT.68K
	mkdir -p "$OUT/guest/home/guest/CPM/A"
	for f in "$S"/cpm/dist/*; do
		n=$(basename "$f")
		l=$(echo "$n" | tr A-Z a-z)
		ln -s "/cpm/dist/$n" "$OUT/guest/home/guest/CPM/A/$l"
	done
	for f in "$S"/cpm/dist/*.REL; do
		n=$(basename "$f" .REL)
		l=$OUT/guest/home/guest/CPM/A/$(echo "$n" | tr A-Z a-z).68k
		[ -h "$l" ] || ln -s "/cpm/dist/$n.REL" "$l"
	done
	python3 -c "import sys; sys.stdout.buffer.write(bytes([0x60, 0x1a, 0, 0, 0, 8] + [0] * 22 +
		[0x30, 0x3c, 0, 0x7f, 0x72, 0, 0x4e, 0x43] + [0] * 8))" > "$OUT/guest/home/guest/CPM/A/exit.68k"
	chmod 755 "$OUT/guest/home/guest/CPM" "$OUT/guest/home/guest/CPM/A"
	chmod 644 "$OUT/guest/home/guest/CPM/A/exit.68k"
	echo "[ok] CP/M-68K: startcpm, $(ls "$S/cpm/dist" | wc -l | tr -d ' ') files for A:"
else
	echo "[skip] CP/M-68K: no $CPMZIP"
fi
# the archive carries its parents: they must not shut others out
chmod 755 "$S"
(cd "$S" && find . -depth -print | cpio -o -H newc -R 0:3 --quiet) > "$OUT/cpm.cpio"
mkdir -p "$OUT/guest"
(cd "$OUT/guest" && find home/guest/CPM -print 2> /dev/null |
	cpio -o -H newc -R 100:1 --quiet) > "$OUT/guest.cpio"
rm -rf "$S" "$OUT/guest"
