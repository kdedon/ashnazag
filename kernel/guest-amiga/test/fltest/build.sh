#!/bin/sh
# build.sh outdir: the fltest Amiga executable
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../../../.." && pwd)
mkdir -p "$1"
OUT=$(CDPATH= cd -- "$1" && pwd)
nice -n 19 "$ROOT/toolchain/linux/bin/m68k-linux-gnu-gcc" -m68020 -Os -ffreestanding -fno-builtin -fno-pic -fno-stack-protector -fomit-frame-pointer -fno-tree-loop-distribute-patterns -Wall -Werror -c "$HERE/fltest.c" -o "$OUT/fltest.o"
"$ROOT/toolchain/bin/m68k-elf-as" -m68020 "$HERE/start.s" -o "$OUT/start.o"
printf '%s\n' 'SECTIONS { .text : { *(.text*) *(.rodata*) *(.data*) *(.bss*) *(COMMON) } /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) } }' > "$OUT/link.ld"
"$ROOT/toolchain/bin/m68k-elf-ld" -r -T "$OUT/link.ld" "$OUT/start.o" "$OUT/fltest.o" -o "$OUT/fltest.elf"
python3 "$ROOT/kernel/guest/amiga/dos/elf2hunk.py" "$OUT/fltest.elf" "$OUT/fltest"
rm -f "$OUT/fltest.o" "$OUT/start.o" "$OUT/link.ld" "$OUT/fltest.elf"
