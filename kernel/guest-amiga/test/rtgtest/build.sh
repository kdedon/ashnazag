#!/bin/sh
# build.sh outdir: the rtgtest Amiga executable
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../../../.." && pwd)
mkdir -p "$1"
OUT=$(CDPATH= cd -- "$1" && pwd)
nice -n 19 "$ROOT/toolchain/linux/bin/m68k-linux-gnu-gcc" -m68020 -Os -ffreestanding -fno-builtin -fno-pic -fno-stack-protector -fomit-frame-pointer -fno-tree-loop-distribute-patterns -Wall -Werror -c "$HERE/rtgtest.c" -o "$OUT/rtgtest.o"
sed 's/fltest/rtgtest/' "$HERE/../fltest/start.s" > "$OUT/start.s"
"$ROOT/toolchain/bin/m68k-elf-as" -m68020 "$OUT/start.s" -o "$OUT/start.o"
printf '%s\n' 'SECTIONS { .text : { *(.text*) *(.rodata*) *(.data*) *(.bss*) *(COMMON) } /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) } }' > "$OUT/link.ld"
"$ROOT/toolchain/bin/m68k-elf-ld" -r -T "$OUT/link.ld" "$OUT/start.o" "$OUT/rtgtest.o" -o "$OUT/rtgtest.elf"
python3 "$ROOT/kernel/guest/amiga/dos/elf2hunk.py" "$OUT/rtgtest.elf" "$OUT/rtgtest"
rm -f "$OUT/rtgtest.o" "$OUT/start.o" "$OUT/start.s" "$OUT/link.ld" "$OUT/rtgtest.elf"
