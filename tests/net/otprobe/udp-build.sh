#!/bin/sh
set -eu
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
TC=$AUX/toolchain/amix
SYS=$TC/m68k-cbm-sysv4/sysroot
W=${1:?private output directory required}
mkdir -p "$W"
nice -n19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -Wall -Wno-comment -Wno-implicit -D__STDC__=0 -c "$D/udp-echo.c" -o "$W/udp-echo.o"
mkdir -p "$W/extra"
(cd "$W/extra" && ar x "$SYS/usr/ccs/lib/libc.so" && rm -f libc.so.1 && ar rc ../libextra.a *.o)
LIBGCC=$(ls "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a | tail -1)
nice -n19 "$TC/bin/m68k-cbm-sysv4-ld" -o "$W/otudp" "$SYS/usr/ccs/lib/crt1.o" "$SYS/usr/ccs/lib/crti.o" \
    "$W/udp-echo.o" -L"$SYS/usr/lib" -lsocket -lnsl "$SYS/usr/lib/libc.so.1" \
    "$W/libextra.a" "$LIBGCC" "$SYS/usr/ccs/lib/crtn.o"

python3 "$D/udp-link-test.py" "$W/otudp"
