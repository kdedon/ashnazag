#!/bin/sh
set -eu
D=$(cd "$(dirname "$0")/.." && pwd)
AUX=$(cd "$D/../../.." && pwd)
W=${1:?private output directory required}
mkdir -p "$W"
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
nice -n19 m68k-cbm-sysv4-gcc -O -m68020 -fcall-used-d2 -Wall -Werror \
    -c "$D/test/pointer-abi.c" -o "$W/pointer-abi.o"
python3 "$D/test/pointer-abi.py" "$AUX/toolchain/linux/bin/m68k-linux-gnu-objdump" "$W/pointer-abi.o"
