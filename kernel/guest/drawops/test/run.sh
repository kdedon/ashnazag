#!/bin/sh
# run.sh -- the drawops tests under Linux user-mode 68k emulation, as
# 68030, 68040 and 68060 (C built for the 68060: no 64-bit multiplies).
# QEMU_M68K names the emulator (qemu-m68k).
set -e
H=$(cd "$(dirname "$0")" && pwd)
AUX=${AUX:-$(cd "$H/../../../.." && pwd)}
B=$AUX/toolchain/linux/bin
Q=${QEMU_M68K:-qemu-m68k}
O=${1:-$H/out}
mkdir -p "$O"
nice -n 19 "$B/m68k-linux-gnu-gcc" -m68060 -O2 -nostdlib -static -ffreestanding -fno-pic -fno-pie \
	-fno-builtin -Wall -Wl,-z,noexecstack -o "$O/test" "$H/start.s" "$H/test.c" "$H/ref.c" "$H/../drawops.s"
# the emulator's 68060 lacks MOVE16: there the library runs its 68030 rows
for c in 30:30 40:40 60:30; do
	printf 'cpu %s: ' ${c%:*}
	nice -n 19 "$Q" -cpu m680${c%:*} "$O/test" ${c#*:} ${COUNT:-2000} | tail -1
done
