#!/bin/sh
# build.sh -- compile the echo service pingd, an AMIX program linked
# against the shared libc.
#
#   sh kernel/net/build.sh [outdir]
#
# outdir defaults to kernel/build/net.  Fails on compiler warnings.
set -e

D=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$D/.." && pwd)
AUX=$(cd "$K/.." && pwd)
TC=$AUX/toolchain/amix
SYS=$TC/m68k-cbm-sysv4/sysroot
LIB=$SYS/usr/ccs/lib
LIBGCC=$(ls "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a | tail -1)
OUT="${1:-$K/build/net}"
mkdir -p "$OUT"
# the archive-only libc members
if [ ! -f "$OUT/libextra.a" ]; then
	rm -rf "$OUT/extra"; mkdir -p "$OUT/extra"
	(cd "$OUT/extra" && ar x "$LIB/libc.so" && rm -f libc.so.1 &&
	 ar rc ../libextra.a $(ar t "$LIB/libc.so" | grep -v '^libc.so.1$'))
	rm -rf "$OUT/extra"
fi
TMPDIR="$OUT" nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -Wall -Wno-comment -D__STDC__=0 -I"$D" \
	-c "$D/pingd.c" -o "$OUT/pingd.o" 2> "$OUT/pingd.warn" || {
	cat "$OUT/pingd.warn"; echo "[FAIL] pingd.c"; exit 1; }
if [ -s "$OUT/pingd.warn" ]; then
	cat "$OUT/pingd.warn"; echo "[FAIL] pingd.c: warnings"; exit 1
fi
nice -n 19 "$TC/bin/m68k-cbm-sysv4-ld" -o "$OUT/pingd" "$LIB/crt1.o" "$LIB/crti.o" \
	"$OUT/pingd.o" "$SYS/usr/lib/libc.so.1" "$OUT/libextra.a" "$LIBGCC" "$LIB/crtn.o"
echo "[OK] $OUT/pingd"
