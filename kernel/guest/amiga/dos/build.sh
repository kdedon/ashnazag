#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../../../.." && pwd)
[ "$#" -eq 1 ] || { echo 'usage: build.sh output-directory' >&2; exit 2; }
mkdir -p "$1"
OUT=$(CDPATH= cd -- "$1" && pwd)
TMP=$(mktemp -d "$OUT/.dos-build.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
nice -n 19 "$ROOT/toolchain/linux/bin/m68k-linux-gnu-gcc" -m68020 -Os -ffreestanding -fno-builtin -fno-pic -fno-stack-protector -fomit-frame-pointer -fpack-struct=2 -Wall -Wextra -Werror -c "$HERE/handler.c" -o "$TMP/handler.o"
"$ROOT/toolchain/bin/m68k-elf-as" -m68020 "$HERE/exec.s" -o "$TMP/exec.o"
printf '%s\n' 'SECTIONS { .text : { *(.text*) *(.rodata*) *(.data*) *(.bss*) } /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) } }' > "$TMP/link.ld"
"$ROOT/toolchain/bin/m68k-elf-ld" -r -T "$TMP/link.ld" "$TMP/exec.o" "$TMP/handler.o" -o "$TMP/dos.o"
python3 "$HERE/elf2hunk.py" "$TMP/dos.o" "$OUT/container-handler"
"$ROOT/toolchain/bin/m68k-elf-as" -m68020 "$HERE/boot.s" -o "$TMP/boot.o"
python3 "$HERE/elf2hunk.py" "$TMP/boot.o" "$OUT/container-boot-hook"
python3 "$HERE/check.py" "$OUT/container-handler" "$OUT/container-boot-hook"
python3 "$HERE/romdata.py" "$OUT/container-handler" "$TMP"
"$ROOT/toolchain/bin/m68k-elf-as" -m68020 -I "$TMP" "$HERE/bootrom.s" -o "$TMP/bootrom.o"
# ROM code: nothing writable
nice -n 19 "$ROOT/toolchain/linux/bin/m68k-linux-gnu-gcc" -m68020 -Os -ffreestanding -fno-builtin -fno-pic -fno-stack-protector -fomit-frame-pointer -fno-tree-loop-distribute-patterns -fno-strict-aliasing -fno-asynchronous-unwind-tables -Wall -Wextra -Werror -c "$HERE/early.c" -o "$TMP/early.o"
"$ROOT/toolchain/bin/m68k-elf-as" -m68020 "$HERE/early.s" -o "$TMP/earlys.o"
printf '%s\n' 'SECTIONS { . = 0xf00000; .text : { *(.text*) *(.rodata*) } extension_payload_end = .; .data : { *(.data*) *(.bss*) *(COMMON) } /DISCARD/ : { *(.comment) *(.note*) } } ASSERT(SIZEOF(.data) == 0, "boot extension: writable data")' > "$TMP/rom.ld"
"$ROOT/toolchain/bin/m68k-elf-ld" -z noexecstack -T "$TMP/rom.ld" "$TMP/bootrom.o" "$TMP/boot.o" "$TMP/earlys.o" "$TMP/early.o" -o "$TMP/rom.elf"
"$ROOT/toolchain/bin/m68k-elf-objcopy" -O binary --gap-fill 0 --pad-to 0xf80000 "$TMP/rom.elf" "$OUT/container-boot.rom"
python3 "$HERE/checkrom.py" "$OUT/container-boot.rom"
