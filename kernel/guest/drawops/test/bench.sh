#!/bin/sh
# bench.sh -- instructions per operation (bench.c's cases), counted with
# one instruction per translation block under user-mode emulation:
# a run with the operation less a run without.  CPU 30 or 40.
set -e
H=$(cd "$(dirname "$0")" && pwd)
AUX=${AUX:-$(cd "$H/../../../.." && pwd)}
B=$AUX/toolchain/linux/bin
Q=${QEMU_M68K:-qemu-m68k}
O=${1:-$H/out}
mkdir -p "$O"
nice -n 19 "$B/m68k-linux-gnu-gcc" -m68060 -O2 -nostdlib -static -ffreestanding -fno-pic -fno-pie \
	-fno-builtin -Wl,-z,noexecstack -o "$O/bench" "$H/start.s" "$H/bench.c" "$H/../drawops.s"
n() { nice -n 19 "$Q" -cpu m68040 -one-insn-per-tb -d exec,nochain -D /dev/stdout "$O/bench" $1 $2 $3 | grep -c '^Trace'; }
for c in ${CASES:-1 2 3 4 5 6 7 8 9 10 11 12 13 21 22 23 24 25 26 27 28 29}; do
	for cpu in ${CPUS:-30 40}; do
		printf '%s/%s: %s  ' $c $cpu $(($(n $cpu $c 1) - $(n $cpu $c 0)))
	done
	echo
done
