#!/bin/sh
# mkcpm.sh -- the CP/M-68K environment's files: startcpm, the CCP and
# the nine distribution disks' files, the shared read-only P:.
#
#   sh images/cpmenv/mkcpm.sh outdir
#
# Out: outdir/root: startcpm, the CCP (/cpm/sys/CCP.68K) and the
# utilities on P: (/cpm/dist); outdir/dri/cpm/dist, DRI's files, which
# stay out of packages.  CPMZIP names DRI's release zip (default:
# media/cpm68k/68kv1_3.zip), which the build needs; without it both are empty.
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
mkdir -p "$OUT/root" "$OUT/dri"
OUT=$(cd "$OUT" && pwd)
S=$OUT/dri
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
	R=$OUT/root
	mkdir -p "$R/usr/bin" "$R/cpm/sys" "$R/cpm/dist" "$S/cpm/dist" "$OUT/zip"
	cp "$B/bin/startcpm" "$R/usr/bin/"
	cp "$B/sys/CCP.68K" "$R/cpm/sys/"
	cp "$B"/dist/* "$S/cpm/dist/"
	# the other disks' files; a name already there (disk 1's) wins
	unzip -q -o "$CPMZIP" -d "$OUT/zip"
	for f in "$OUT"/zip/DISK[2-9]/*; do
		n=$(basename "$f")
		[ "$n" = CPM.SYS ] || [ -f "$S/cpm/dist/$n" ] || cp "$f" "$S/cpm/dist/"
	done
	# ours to the package; they win over DRI's names
	for n in EXIT.68K MOUNT.68K GET.RSX PUT.RSX HELP.HLP \
	    $(cd "$AUX/kernel/guest/cpm/cmd" && ls *.c | sed 's/\.c$/.68K/' | tr a-z A-Z); do
		mv "$S/cpm/dist/$n" "$R/cpm/dist/"
	done
	sh "$AUX/tests/cpm/links.sh" "$S/cpm/dist"
	(cd "$R/cpm/dist" && rm -f $(ls | sed "s|^|$S/cpm/dist/|"))
	rm -rf "$OUT/b" "$OUT/zip"
	chmod -R a+rX,go-w "$S" "$R"
	chmod 755 "$R/usr/bin/startcpm"
	chmod 444 "$R/cpm/sys/CCP.68K" "$R"/cpm/dist/* "$S"/cpm/dist/*
	echo "[ok] CP/M-68K: startcpm, $(ls "$S/cpm/dist" | wc -l | tr -d ' ') files for P:"
else
	echo "[skip] CP/M-68K: no $CPMZIP"
fi
