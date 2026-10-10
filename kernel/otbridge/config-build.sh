#!/bin/sh
set -eu
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
OUT=$(mkdir -p "$1" && cd "$1" && pwd)
TC=$AUX/toolchain/amix
SYS=$TC/m68k-cbm-sysv4/sysroot
mkdir -p "$OUT/extra" "$OUT/tmp"
export TMPDIR="$OUT/tmp"
(cd "$OUT/extra" && ar x "$SYS/usr/ccs/lib/libc.so" && rm -f libc.so.1 && ar rc "$OUT/libextra.a" *.o)
for f in otbconfig config anchor; do
 nice -n19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -m68020 -D__STDC__=0 -Wall -I"$D" -c "$D/$f.c" -o "$OUT/$f-user.o"
done
LIBGCC=$(ls "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a | tail -1)
nice -n19 "$TC/bin/m68k-cbm-sysv4-ld" -o "$OUT/otbconfig" \
 "$SYS/usr/ccs/lib/crt1.o" "$SYS/usr/ccs/lib/crti.o" \
 "$OUT/otbconfig-user.o" "$OUT/config-user.o" "$OUT/anchor-user.o" \
 -L"$SYS/usr/lib" -lsocket -lnsl "$SYS/usr/lib/libc.so.1" \
 "$OUT/libextra.a" "$LIBGCC" "$SYS/usr/ccs/lib/crtn.o"
