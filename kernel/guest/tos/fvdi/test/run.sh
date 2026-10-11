#!/bin/sh
# run.sh -- the frame buffer drivers' drawing tests: pltest.c under
# Linux user-mode 68k emulation as 68030, 68040 and 68060, bltest.c on
# the host.  QEMU_M68K names the emulator (qemu-m68k).
set -e
H=$(cd "$(dirname "$0")" && pwd)
AUX=${AUX:-$(cd "$H/../../../../.." && pwd)}
B=$AUX/toolchain/linux/bin
Q=${QEMU_M68K:-qemu-m68k}
D=$H/../../../drawops
O=${1:-$H/out}
mkdir -p "$O"
nice -n 19 "$B/m68k-linux-gnu-gcc" -m68060 -O2 -nostdlib -static -ffreestanding -fno-pic -fno-pie \
	-fno-builtin -Wall -Wl,-z,noexecstack -o "$O/pltest" "$D/test/start.s" "$H/pltest.c" \
	"$H/../plane.c" "$D/test/ref.c" "$D/drawops.s"
for c in 30:30 40:40 60:30; do
	printf 'cpu %s: ' ${c%:*}
	nice -n 19 "$Q" -cpu m680${c%:*} "$O/pltest" ${c#*:} ${COUNT:-300} | tail -1
done
# the blitter registers for 1-bit fills and copies, on a model of the blitter
nice -n 19 ${CC:-cc} -O2 -Wall -o "$O/bltest" "$H/bltest.c" "$D/test/ref.c"
"$O/bltest" ${COUNT:-20000}
