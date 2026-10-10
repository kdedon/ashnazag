#!/bin/sh
set -eu
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
LX=$AUX/toolchain/linux/bin/m68k-linux-gnu
mkdir -p "$1"
nice -n19 "$LX-gcc" -m68020 -mpcrel -O2 -ffreestanding -fno-builtin -ffixed-a5 -nostdlib \
    -Wall -Werror -Wl,-e,entry -Wl,-Ttext=0 -Wl,--build-id=none -Wl,--emit-relocs \
    -o "$1/probe.elf" "$D/probe.s" "$D/probe.c"
python3 "$D/checkelf.py" "$1/probe.elf"
"$LX-objcopy" -O binary -j .text -j .rodata "$1/probe.elf" "$1/probe.bin"
python3 "$D/mkapp.py" "$1/probe.bin" "$1/OTProbe"
