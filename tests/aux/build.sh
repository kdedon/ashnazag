#!/bin/sh
# build.sh -- t_aux's inputs: the guest modules, built against the
# kernel under test, A/UX programs and files from a local A/UX root,
# and abi, a freestanding A/UX program built here.
#
#   sh tests/aux/build.sh kernel.elf outdir
#
# Out: outdir/mod.d/{guestcore,auxcore,auxexec}, outdir/bin/<program>,
# outdir/root/<path> (shared libraries, terminfo, termcap).
# AUXROOT names the A/UX root (proprietary, never in the repository).
# A kernel without guest support gets nothing, and t_aux skips.
set -e

T=$(cd "$(dirname "$0")/.." && pwd)
AUX=$(cd "$T/.." && pwd)
KDIR=${KDIR:-$AUX/kernel}
# A/UX root: $AUXROOT, else the path in tests/aux/auxroot, else /aux
[ -n "$AUXROOT" ] || [ ! -f "$T/aux/auxroot" ] || AUXROOT=$(cat "$T/aux/auxroot")
AUXROOT=${AUXROOT:-/aux}
KERNEL=$1
OUT=$2
PATH="$AUX/toolchain/bin:$PATH"
export PATH

rm -rf "$OUT"
if ! m68k-elf-nm "$KERNEL" | grep -q ' T guest_trap$'; then
	echo "[skip] $KERNEL has no guest support"
	exit 0
fi
sh "$KDIR/guest/build.sh" -m "$KERNEL" "$OUT"
mkdir -p "$OUT/bin"
for p in sh ls cp mv rm rmdir awk more sleep; do
	[ -f "$AUXROOT/bin/$p" ] && cp "$AUXROOT/bin/$p" "$OUT/bin/$p"
done
[ -f "$AUXROOT/usr/bin/vi" ] && cp "$AUXROOT/usr/bin/vi" "$OUT/bin/vi"
for f in shlib/libc1_s usr/lib/terminfo/v/vt100 etc/termcap; do
	if [ -f "$AUXROOT/$f" ]; then
		mkdir -p "$OUT/root/$(dirname "$f")"
		cp "$AUXROOT/$f" "$OUT/root/$f"
	fi
done

TC=$AUX/toolchain/amix
O=$OUT/obj
mkdir -p "$O"
LIBGCC=$(ls "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a | tail -1)
nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -Wall -m68020 -fno-builtin \
	-c "$T/aux/abi.c" -o "$O/abi.o"
"$TC/bin/m68k-cbm-sysv4-as" -o "$O/abi0.o" "$T/aux/abi0.s"
"$TC/bin/m68k-cbm-sysv4-ld" -T "$T/aux/abi.ld" -o "$O/abi.elf" "$O/abi0.o" "$O/abi.o" \
	"$LIBGCC"
python3 "$T/aux/elf2aux.py" "$O/abi.elf" "$OUT/bin/abi"
echo "[ok] $(ls "$OUT/mod.d" | wc -l) modules, $(ls "$OUT/bin" | wc -l) A/UX programs"
