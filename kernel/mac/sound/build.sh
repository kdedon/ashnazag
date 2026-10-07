#!/bin/sh
# build.sh -- compile the sound chip device and the sound service.
#
#   sh kernel/mac/sound/build.sh [outdir]
#
# outdir defaults to kernel/build/mac/sound.  Produces snd.o and
# auxsnd.o for the sound modules (mods.sh), and sndd, sndaux and sndtest,
# AMIX programs linked against the shared libc.
# Fails on compiler warnings, common symbols and sections the AMIX
# loader does not bind.
set -e

D=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$D/../.." && pwd)
AUX=$(cd "$K/.." && pwd)
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
PATH="$AUX/toolchain/linux/bin:$AUX/toolchain/bin:$PATH"
export PATH

OUT="${1:-$K/build/mac/sound}"
mkdir -p "$OUT"
for f in snd auxsnd; do
nice -n 19 m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS $AMIX_DIAG_CFLAGS -m68040 -fno-common -Wall -Wno-comment \
	-I"$D" -I"$K/mac/display" -c "$D/$f.c" -o "$OUT/$f.o" 2> "$OUT/$f.warn" || {
	cat "$OUT/$f.warn"; echo "[FAIL] $f.c"; exit 1; }
if [ -s "$OUT/$f.warn" ]; then
	cat "$OUT/$f.warn"; echo "[FAIL] $f.c: warnings"; exit 1
fi
bad=$(m68k-linux-gnu-readelf -S "$OUT/$f.o" |
	awk '$1 ~ /^\[/ { n = ($1 == "[") ? $3 : $2;
	     if (n ~ /^\./ && n !~ /^\.(text|data|bss|comment|rela\.text|rela\.data|shstrtab|symtab|strtab)$/) print n }')
[ -z "$bad" ] || { echo "[FAIL] $f.o has sections: $bad"; exit 1; }
if m68k-linux-gnu-nm "$OUT/$f.o" | grep -q ' C '; then
	echo "[FAIL] $f.o has common symbols"; exit 1
fi
echo "[OK] $OUT/$f.o"
done

# sndd: user program, shared libc plus the archive-only members
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
for f in sndd sndaux sndtest; do
TMPDIR="$OUT" nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -Wall -Wno-comment -D__STDC__=0 -I"$D" \
	-c "$D/$f.c" -o "$OUT/$f.o" 2> "$OUT/$f.warn" || {
	cat "$OUT/$f.warn"; echo "[FAIL] $f.c"; exit 1; }
if [ -s "$OUT/$f.warn" ]; then
	cat "$OUT/$f.warn"; echo "[FAIL] $f.c: warnings"; exit 1
fi
nice -n 19 "$TC/bin/m68k-cbm-sysv4-ld" -o "$OUT/$f" "$LIB/crt1.o" "$LIB/crti.o" \
	"$OUT/$f.o" "$SYS/usr/lib/libc.so.1" "$OUT/libextra.a" "$LIBGCC" "$LIB/crtn.o"
echo "[OK] $OUT/$f"
done
