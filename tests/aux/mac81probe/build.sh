#!/bin/sh
set -eu
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
LX=$AUX/toolchain/linux/bin/m68k-linux-gnu
mkdir -p "$1"
nice -n 19 "$LX-gcc" -m68020 -mpcrel -ffixed-a5 -O2 -ffreestanding -fno-builtin \
    -Wall -Werror -c -o "$1/probe.o" "$D/probe.c"
if "$LX-objdump" -r "$1/probe.o" | grep -q R_68K_32; then
    echo 'Mac81Probe: absolute relocation'; exit 1
fi
nice -n 19 "$LX-gcc" -m68020 -nostdlib -Wl,-e,entry -Wl,-Ttext=0 \
    -Wl,--build-id=none -o "$1/probe.elf" "$D/probe.s" "$1/probe.o"
"$LX-objcopy" -O binary -j .text -j .rodata "$1/probe.elf" "$1/probe.bin"
python3 "$D/check-glue.py" "$1" "$LX-nm"
python3 "$D/mkapp.py" "$1/probe.bin" "$1/Mac81Probe"
