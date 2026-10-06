#!/bin/sh
# build.sh output-directory: bsdsocket.library for LIBS:
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../../../.." && pwd)
[ "$#" -eq 1 ] || { echo 'usage: build.sh output-directory' >&2; exit 2; }
mkdir -p "$1"
OUT=$(CDPATH= cd -- "$1" && pwd)
TMP=$(mktemp -d "$OUT/.bsdsock-build.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
nice -n 19 "$ROOT/toolchain/linux/bin/m68k-linux-gnu-gcc" -m68020 -Os -ffreestanding -fno-builtin \
    -fno-pic -fno-stack-protector -fomit-frame-pointer -fno-common -Wall -Wextra -Wno-unused-parameter \
    -Werror -I"$ROOT/kernel/guest/include" -c "$HERE/bsdsocket.c" -o "$TMP/bsdsocket.o"
"$ROOT/toolchain/bin/m68k-elf-as" -m68020 "$HERE/lib.s" -o "$TMP/lib.o"
printf '%s\n' 'SECTIONS { .text : { *(.text*) *(.rodata*) *(.data*) *(.bss*) } /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) } }' > "$TMP/link.ld"
"$ROOT/toolchain/bin/m68k-elf-ld" -r -T "$TMP/link.ld" "$TMP/lib.o" "$TMP/bsdsocket.o" -o "$TMP/lib.elf"
python3 "$HERE/../dos/elf2hunk.py" "$TMP/lib.elf" "$OUT/bsdsocket.library"
python3 "$HERE/check.py" "$OUT/bsdsocket.library"
