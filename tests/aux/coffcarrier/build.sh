#!/bin/sh
set -eu
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
LX=$AUX/toolchain/linux/bin/m68k-linux-gnu
mkdir -p "$1"
nice -n 19 "$LX-gcc" -m68020 -mpcrel -ffixed-a5 -ffixed-a4 -O2 -ffreestanding -fno-builtin \
    -Wall -Werror -c -o "$1/carrier.o" "$D/carrier.c"
if "$LX-objdump" -r "$1/carrier.o" | grep -q R_68K_32; then
    echo 'COFFCarrier: absolute relocation'; exit 1
fi
nice -n 19 "$LX-gcc" -m68020 -nostdlib -Wl,-e,entry -Wl,-Ttext=0 \
    -Wl,--build-id=none -o "$1/carrier.elf" "$D/carrier.s" "$1/carrier.o"
"$LX-objcopy" -O binary -j .text -j .rodata "$1/carrier.elf" "$1/carrier.bin"
if [ -n "${COFFRSRC:-}" ]; then
    python3 "$D/mkapp.py" "$1/carrier.bin" "$1/COFFCarrier" "$COFFRSRC"
    python3 "$D/check-rsrc.py" "$1/COFFCarrier" "$COFFRSRC"
else
    python3 "$D/mkapp.py" "$1/carrier.bin" "$1/COFFCarrier"
fi
python3 "$D/check-fork.py" "$1/carrier.elf"
