#!/bin/sh
# build.sh outdir: the w3dtest Amiga executable
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../../../.." && pwd)
mkdir -p "$1"
OUT=$(CDPATH= cd -- "$1" && pwd)
nice -n 19 "$ROOT/toolchain/linux/bin/m68k-linux-gnu-gcc" -m68020 -m68881 -Os -ffreestanding -fno-builtin -fno-pic -fno-stack-protector -fomit-frame-pointer -fno-tree-loop-distribute-patterns -Wall -Werror -c "$HERE/w3dtest.c" -o "$OUT/w3dtest.o"
sed 's/fltest/w3dtest/' "$HERE/../fltest/start.s" > "$OUT/start.s"
"$ROOT/toolchain/bin/m68k-elf-as" -m68020 "$OUT/start.s" -o "$OUT/start.o"
printf '%s\n' 'SECTIONS { .text : { *(.text*) *(.rodata*) *(.data*) *(.bss*) *(COMMON) } /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) } }' > "$OUT/link.ld"
"$ROOT/toolchain/bin/m68k-elf-ld" -r -T "$OUT/link.ld" "$OUT/start.o" "$OUT/w3dtest.o" -o "$OUT/w3dtest.elf"
python3 "$ROOT/kernel/guest/amiga/dos/elf2hunk.py" "$OUT/w3dtest.elf" "$OUT/w3dtest"
rm -f "$OUT/w3dtest.o" "$OUT/start.o" "$OUT/start.s" "$OUT/link.ld" "$OUT/w3dtest.elf"
