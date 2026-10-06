#!/bin/sh
set -eu
G=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
AUX=$(CDPATH= cd -- "$G/../.." && pwd)
if [ "$#" -ne 1 ]; then
    echo 'usage: build.sh output-directory' >&2
    exit 2
fi
mkdir -p "$1"
OUT=$(CDPATH= cd -- "$1" && pwd)
O=$(mktemp -d "$OUT/.build.XXXXXX")
trap 'rm -rf "$O"' EXIT HUP INT TERM
TC=$AUX/toolchain/amix
SYS=$TC/m68k-cbm-sysv4/sysroot
LIBGCC=$(ls "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a | tail -1)
mkdir "$O/extra"
(cd "$O/extra" && ar x "$SYS/usr/ccs/lib/libc.so" && rm -f libc.so.1 &&
 ar rc ../libextra.a $(ar t "$SYS/usr/ccs/lib/libc.so" | grep -v '^libc.so.1$'))
for source in amiga/startmig amiga/migdisp amiga/rtgtransport amiga/input amiga/hostfs \
    amiga/hostfsbroker amiga/sysroot amiga/miglog amiga/migsnd sndout; do
    nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -Wall -D__STDC__=0 \
        -I"$G/include" -I"$G/amiga" -I"$AUX/kernel/mac/display" -I"$AUX/kernel/mac/sound" \
        -c "$G/$source.c" -o "$O/${source#amiga/}.o"
done
nice -n 19 "$TC/bin/m68k-cbm-sysv4-ld" -o "$OUT/startmig" \
    "$SYS/usr/ccs/lib/crt1.o" "$SYS/usr/ccs/lib/crti.o" "$O/startmig.o" "$O/migdisp.o" "$O/rtgtransport.o" "$O/input.o" "$O/hostfs.o" "$O/hostfsbroker.o" "$O/sysroot.o" "$O/miglog.o" "$O/migsnd.o" "$O/sndout.o" \
    "$SYS/usr/lib/libc.so.1" "$O/libextra.a" "$LIBGCC" "$SYS/usr/ccs/lib/crtn.o"
echo "[ok] $OUT/startmig"
