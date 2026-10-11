#!/bin/sh
set -eu
D=$(cd "$(dirname "$0")" && pwd)
A=$(cd "$D/../../.." && pwd)
TC=${TC:-$A/toolchain/amix}
SYS=$TC/m68k-cbm-sysv4/sysroot
O=${1:?usage: build.sh OUTDIR [DESTDIR]}
mkdir -p "$O"
nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -Wall -D__STDC__=0 -I"$D/../include" -c "$D/startnative.c" -o "$O/startnative.o"
for LIBGCC in "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a; do
    [ -f "$LIBGCC" ] && break
done
[ -f "$LIBGCC" ] || { echo "libgcc.a not found" >&2; exit 1; }
nice -n 19 "$TC/bin/m68k-cbm-sysv4-ld" -o "$O/startnative" "$SYS/usr/ccs/lib/crt1.o" "$SYS/usr/ccs/lib/crti.o" "$O/startnative.o" "$SYS/usr/lib/libc.so.1" "$LIBGCC" "$SYS/usr/ccs/lib/crtn.o"
if [ "$#" -gt 1 ]; then
    mkdir -p "$2/usr/bin"
    cp "$O/startnative" "$2/usr/bin/startnative"
    chmod 755 "$2/usr/bin/startnative"
    ln -sf startnative "$2/usr/bin/startsmsqe"
    ln -sf startnative "$2/usr/bin/startlisa"
    ln -sf startnative "$2/usr/bin/startvoyage200"
fi
