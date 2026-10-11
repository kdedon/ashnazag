#!/bin/sh
set -eu
D=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
K=$(CDPATH= cd -- "$D/../../.." && pwd)
TC=${AMIX_TC:?set AMIX_TC to the installed AMIX toolchain}
OUT=${1:-/tmp/native-audio-clean}
SYS=$TC/m68k-cbm-sysv4/sysroot
LIB=$SYS/usr/ccs/lib
mkdir -p "$OUT"
for source in "$D/../nativehost_audio.c" "$D/fixture.c"; do
    name=$(basename "$source" .c)
    nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -Wall -Wno-comment -D__STDC__=0 \
        -I"$D/.." -I"$K/mac/sound" -c "$source" -o "$OUT/$name.o" \
        2> "$OUT/$name.warn"
    if test -s "$OUT/$name.warn"; then
        cat "$OUT/$name.warn"
        exit 1
    fi
done
mkdir -p "$OUT/libextra"
(cd "$OUT/libextra" && ar x "$LIB/libc.so" && rm -f libc.so.1 && ar rc "$OUT/libextra.a" ./*.o)
LIBGCC=$(find "$TC/lib/gcc-lib/m68k-cbm-sysv4" -name libgcc.a | head -1)
nice -n 19 "$TC/bin/m68k-cbm-sysv4-ld" -o "$OUT/t_native_audio" \
    "$LIB/crt1.o" "$LIB/crti.o" "$OUT/fixture.o" "$OUT/nativehost_audio.o" \
    "$SYS/usr/lib/libc.so.1" "$OUT/libextra.a" "$LIBGCC" "$LIB/crtn.o"
echo "$OUT/t_native_audio"
