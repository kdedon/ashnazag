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
# Warp3D.library's host side, with Mesa's software renderer when it is
# built, and its DRI drivers when they are
GLW=${GLW:-$AUX/images/work/gl}
TC95=$AUX/toolchain/amix-gcc295
W3D= GL= DRI= DRILIB=
if [ -f "$GLW/lib/libdrihost.a" ]; then
    DRI="-DW3D_DRI -I$AUX/kernel/mac/display"
    DRILIB=$GLW/lib/libdrihost.a
fi
if [ -f "$GLW/lib/libOSMesa.a" ]; then
    [ -x "$TC95/bin/m68k-cbm-sysv4-gcc" ] || { echo "[FAIL] Mesa is built but $TC95 is missing" >&2; exit 1; }
    nice -n 19 "$TC95/bin/m68k-cbm-sysv4-gcc" -O2 -m68040 -fomit-frame-pointer -Wall $DRI \
        -I"$GLW/include" -I"$G/amiga" -c "$G/amiga/w3d.c" -o "$O/w3d.o"
    nice -n 19 "$TC95/bin/m68k-cbm-sysv4-gcc" -O2 -m68040 -fomit-frame-pointer -Wall \
        -c "$G/amiga/fastmem.c" -o "$O/fastmem.o"
    W3D=-DWARP3D
    GL="$O/fastmem.o $O/w3d.o $DRILIB $GLW/lib/libOSMesa.a $SYS/usr/ccs/lib/libm.a $(ls "$TC95"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a | tail -1)"
fi
for source in amiga/startmig amiga/migdisp amiga/rtgtransport amiga/input amiga/hostfs \
    amiga/hostfsbroker amiga/sysroot amiga/miglog amiga/migsnd amiga/migfloppy amiga/amfm sndout; do
    nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -Wall -D__STDC__=0 $W3D \
        -I"$G/include" -I"$G/amiga" -I"$AUX/kernel/mac/display" -I"$AUX/kernel/mac/sound" \
        -c "$G/$source.c" -o "$O/${source#amiga/}.o"
done
nice -n 19 "$TC/bin/m68k-cbm-sysv4-ld" -o "$OUT/startmig" \
    "$SYS/usr/ccs/lib/crt1.o" "$SYS/usr/ccs/lib/crti.o" "$O/startmig.o" "$O/migdisp.o" "$O/rtgtransport.o" "$O/input.o" "$O/hostfs.o" "$O/hostfsbroker.o" "$O/sysroot.o" "$O/miglog.o" "$O/migsnd.o" "$O/migfloppy.o" "$O/amfm.o" "$O/sndout.o" $GL \
    "$SYS/usr/lib/libc.so.1" "$O/libextra.a" "$LIBGCC" "$SYS/usr/ccs/lib/crtn.o"
echo "[ok] $OUT/startmig"
