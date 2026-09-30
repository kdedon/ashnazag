#!/bin/sh
# build.sh -- assemble the boot blocks and build mkbb.
#
#   sh kernel/mac/bootblk/build.sh [outdir]
#
# outdir defaults to kernel/mac/bootblk/build: bootblk.bin (1024 bytes,
# parameters empty), bootblk.lst (disassembly) and mkbb.
set -e

D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
T=$AUX/toolchain/bin
OUT=${1:-$D/build}
mkdir -p "$OUT"

nice -n 19 "$T/m68k-elf-as" -m68040 -o "$OUT/bootblk.o" "$D/bootblk.s"
"$T/m68k-elf-objcopy" -O binary "$OUT/bootblk.o" "$OUT/bootblk.bin"
"$T/m68k-elf-objdump" -d "$OUT/bootblk.o" > "$OUT/bootblk.lst"
n=$(wc -c < "$OUT/bootblk.bin")
[ "$n" -eq 1024 ] || { echo "[FAIL] bootblk.bin is $n bytes, not 1024"; exit 1; }
set -- $("$T/m68k-elf-nm" "$OUT/bootblk.o" | awk '$3 == "code_end" || $3 == "cmdline" { print $1 }' | sort)
echo "[OK] $OUT/bootblk.bin: code ends at 0x$1, command line at 0x$2"

nice -n 19 cc -std=gnu89 -O -w -o "$OUT/mkbb" "$D/mkbb.c"
echo "[OK] $OUT/mkbb"
