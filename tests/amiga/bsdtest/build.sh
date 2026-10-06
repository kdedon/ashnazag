#!/bin/sh
# build.sh outdir: bsdtest (Amiga, for SYS:) and bsdsrv (host, with libsocket)
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
AUX=$(CDPATH= cd -- "$HERE/../../.." && pwd)
[ "$#" -eq 1 ] || { echo 'usage: build.sh outdir' >&2; exit 2; }
mkdir -p "$1"
OUT=$(CDPATH= cd -- "$1" && pwd)
TMP=$(mktemp -d "$OUT/.bsdtest-build.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
nice -n 19 "$AUX/toolchain/linux/bin/m68k-linux-gnu-gcc" -m68020 -Os -ffreestanding -fno-builtin \
    -fno-pic -fno-stack-protector -fomit-frame-pointer -fno-common -Wall -Werror \
    -c "$HERE/bsdtest.c" -o "$TMP/bsdtest.o"
"$AUX/toolchain/bin/m68k-elf-as" -m68020 "$HERE/start.s" -o "$TMP/start.o"
printf '%s\n' 'SECTIONS { .text : { *(.text*) *(.rodata*) *(.data*) *(.bss*) } /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) } }' > "$TMP/link.ld"
"$AUX/toolchain/bin/m68k-elf-ld" -r -T "$TMP/link.ld" "$TMP/start.o" "$TMP/bsdtest.o" -o "$TMP/bsdtest.elf"
python3 "$AUX/kernel/guest/amiga/dos/elf2hunk.py" "$TMP/bsdtest.elf" "$OUT/bsdtest"
TC=$AUX/toolchain/amix
SYS=$TC/m68k-cbm-sysv4/sysroot
nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -D__STDC__=0 -c "$HERE/bsdsrv.c" -o "$TMP/bsdsrv.o"
# libc.so's archive members that libsocket wants statically
mkdir "$TMP/extra"
(cd "$TMP/extra" && ar x "$SYS/usr/ccs/lib/libc.so" && rm -f libc.so.1 &&
 ar rc ../libextra.a $(ar t "$SYS/usr/ccs/lib/libc.so" | grep -v '^libc.so.1$'))
"$TC/bin/m68k-cbm-sysv4-ld" -o "$OUT/bsdsrv" "$SYS/usr/ccs/lib/crt1.o" "$SYS/usr/ccs/lib/crti.o" \
    "$TMP/bsdsrv.o" -L"$SYS/usr/lib" -lsocket -lnsl "$SYS/usr/lib/libc.so.1" "$TMP/libextra.a" \
    "$(ls "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a | tail -1)" "$SYS/usr/ccs/lib/crtn.o"
