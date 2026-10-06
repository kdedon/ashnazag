#!/bin/sh
# mkboot.sh OUTDIR -- build the root-sector boot code (bootsec.bin) and
# the AXB loader (axbload.bin) for instboot.py.
# CROSS=prefix selects another m68k toolchain (e.g. m68k-linux-gnu-).
set -e
A=$(cd "$(dirname "$0")" && pwd)
B=${CROSS-$A/../../toolchain/linux/bin/m68k-linux-gnu-}
O=$1
command -v "${B}gcc" >/dev/null ||
	{ echo "[FAIL] no ${B}gcc; set CROSS=<toolchain prefix>"; exit 1; }
mkdir -p "$O"
LD="${B}ld -N -Ttext=0 -z noexecstack --no-warn-rwx-segments"
${B}as -m68030 -o "$O/bootsec.o" "$A/bootsec.s"
$LD -o "$O/bootsec.elf" "$O/bootsec.o"
${B}objcopy -O binary -j .text "$O/bootsec.elf" "$O/bootsec.bin"
${B}as -m68030 -o "$O/axbstart.o" "$A/axbstart.s"
${B}gcc -m68030 -mtune=68060 -Os -mpcrel -ffreestanding -fno-builtin \
	-fno-tree-loop-distribute-patterns -fomit-frame-pointer \
	-Wall -Wno-array-bounds -Werror -c -o "$O/axbload.o" "$A/axbload.c"
$LD -e _start -o "$O/axbload.elf" "$O/axbstart.o" "$O/axbload.o"
${B}objcopy -O binary -j .text -j .rodata "$O/axbload.elf" "$O/axbload.bin"
# position independent, no external references, fits with its data in 8 KB
! ${B}readelf -r "$O/axbload.o" | grep -q R_68K_32 ||
	{ echo "[FAIL] axbload: absolute relocation"; exit 1; }
! ${B}nm -u "$O/axbload.elf" | grep -q . ||
	{ echo "[FAIL] axbload: undefined symbols"; exit 1; }
end=$(${B}nm "$O/axbload.elf" | awk '$3 == "_end" { print $1 }')
[ $((0x$end)) -le 8192 ] && [ $(wc -c < "$O/axbload.bin") -le 7680 ] ||
	{ echo "[FAIL] axbload: too big"; exit 1; }
