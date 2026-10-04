#!/bin/sh
# build.sh -- compile the display service and the dstest tool.
#
#   sh kernel/mac/display/build.sh [outdir]
#
# outdir defaults to kernel/build/mac/display.  Produces ds.o (ds.c,
# dsdev.c, dsseg.c linked -r) for the kernel and dstest, an AMIX
# program linked against the shared libc.  Fails on compiler warnings,
# common symbols and sections the AMIX loader does not bind.
set -e

D=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$D/../.." && pwd)
AUX=$(cd "$K/.." && pwd)
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
PATH="$AUX/toolchain/linux/bin:$AUX/toolchain/bin:$PATH"
export PATH

OUT="${1:-$K/build/mac/display}"
mkdir -p "$OUT"
INC="-I$D -I$K/mac/video -I$K/mac/adb"

objs=
for f in ds dsdev dsseg; do
	nice -n 19 m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS $AMIX_DIAG_CFLAGS -m68040 -fno-common -Wall -Wno-comment $INC \
		-c "$D/$f.c" -o "$OUT/$f.o" 2> "$OUT/$f.warn" || {
		cat "$OUT/$f.warn"; echo "[FAIL] $f.c"; exit 1; }
	if [ -s "$OUT/$f.warn" ]; then
		cat "$OUT/$f.warn"; echo "[FAIL] $f.c: warnings"; exit 1
	fi
	objs="$objs $OUT/$f.o"
done
m68k-cbm-sysv4-ld -r -o "$OUT/ds.new" $objs
mv "$OUT/ds.new" "$OUT/ds.o"
rm -f "$OUT/dsdev.o" "$OUT/dsseg.o"
bad=$(m68k-linux-gnu-readelf -S "$OUT/ds.o" |
	awk '$1 ~ /^\[/ { n = ($1 == "[") ? $3 : $2;
	     if (n ~ /^\./ && n !~ /^\.(text|data|bss|comment|rela\.text|rela\.data|shstrtab|symtab|strtab)$/) print n }')
[ -z "$bad" ] || { echo "[FAIL] ds.o has sections: $bad"; exit 1; }
if m68k-linux-gnu-nm "$OUT/ds.o" | grep -q ' C '; then
	echo "[FAIL] ds.o has common symbols"; exit 1
fi
echo "[OK] $OUT/ds.o"

# dstest: user program, shared libc plus the archive-only members
TC=$AUX/toolchain/amix
SYS=$TC/m68k-cbm-sysv4/sysroot
LIB=$SYS/usr/ccs/lib
LIBGCC=$(ls "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a | tail -1)
if [ ! -f "$OUT/libextra.a" ]; then
	rm -rf "$OUT/extra"; mkdir -p "$OUT/extra"
	(cd "$OUT/extra" && ar x "$LIB/libc.so" && rm -f libc.so.1 &&
	 ar rc ../libextra.a $(ar t "$LIB/libc.so" | grep -v '^libc.so.1$'))
	rm -rf "$OUT/extra"
fi
TMPDIR="$OUT" nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -Wall -Wno-comment -D__STDC__=0 -I"$D" \
	-c "$D/dstest.c" -o "$OUT/dstest.o"
nice -n 19 "$TC/bin/m68k-cbm-sysv4-ld" -o "$OUT/dstest" "$LIB/crt1.o" "$LIB/crti.o" \
	"$OUT/dstest.o" "$SYS/usr/lib/libc.so.1" "$OUT/libextra.a" "$LIBGCC" "$LIB/crtn.o"
echo "[OK] $OUT/dstest"
