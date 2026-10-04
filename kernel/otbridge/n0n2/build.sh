#!/bin/sh
# build.sh -- mkaslm (host and AMIX), the library runtime and the .ENET
# driver image.
#
#   sh kernel/otbridge/n0n2/build.sh [outdir]
#
# Out: mkaslm, mkaslm.amix, aslmrt.o (link it first into a library's
# ld -r object), enet.drvr (DRVR resource data, position independent).
set -e
N=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$N/../../.." && pwd)
OUT=${1:-$N/build}
TB=$AUX/toolchain/bin
NICE="nice -n 19"
mkdir -p "$OUT"
$NICE ${HOSTCC:-cc} -std=gnu89 -O -Wall -o "$OUT/mkaslm" "$N/mkaslm/mkaslm.c"
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
$NICE m68k-cbm-sysv4-gcc -O -m68020 -fcall-used-d2 -o "$OUT/mkaslm.amix" "$N/mkaslm/mkaslm.c"
"$TB/m68k-elf-as" -m68020 -o "$OUT/aslmrt.o" "$N/mkaslm/aslmrt.s"
"$TB/m68k-elf-as" -m68020 -o "$OUT/enet.o" "$N/enet/enet.s"
"$TB/m68k-elf-objcopy" -O binary -j .text "$OUT/enet.o" "$OUT/enet.drvr"
rm -f "$OUT/enet.o"
echo "[ok] $OUT: mkaslm mkaslm.amix aslmrt.o enet.drvr"
