#!/bin/sh
# build.sh -- t_mint's inputs: mintrun and MiNTLib test programs.
#
#   sh tests/mint/build.sh outdir
#
# Out: outdir/root/tos/bin/mintrun, outdir/root/tests/mint/*.prg.
# Without the MiNT cross tools only mintrun is built, and t_mint skips
# the programs.
set -e
T=$(cd "$(dirname "$0")/.." && pwd)
AUX=$(cd "$T/.." && pwd)
KDIR=${KDIR:-$AUX/kernel}
G=$KDIR/guest
OUT=$1
TC=$AUX/toolchain/amix
SYS=$TC/m68k-cbm-sysv4/sysroot
rm -rf "$OUT/root" "$OUT/obj"
O=$OUT/obj
R=$OUT/root
mkdir -p "$O" "$R/tos/bin" "$R/tests/mint"
LIBGCC=$(ls "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a | tail -1)
nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -Wall -Wno-implicit -D__STDC__=0 \
	-I"$G/mod/tosguest" -c "$G/tos/mintrun.c" -o "$O/mintrun.o"
nice -n 19 "$TC/bin/m68k-cbm-sysv4-as" -o "$O/mintent.o" "$G/tos/mintent.s"
nice -n 19 "$TC/bin/m68k-cbm-sysv4-ld" -o "$R/tos/bin/mintrun" "$SYS/usr/ccs/lib/crt1.o" \
	"$SYS/usr/ccs/lib/crti.o" "$O/mintrun.o" "$O/mintent.o" "$SYS/usr/lib/libc.so.1" \
	"$T/build/obj/libextra.a" "$LIBGCC" "$SYS/usr/ccs/lib/crtn.o"
if ! B=$(AUX=$AUX sh "$G/tos/mintgcc.sh" "${MINT:-$O/mint}" 2>"$O/mintgcc.log"); then
	echo "[skip] MiNT programs: no MiNT cross tools"
	exit 0
fi
for p in mhello mcat mproc msock; do
	PATH=$B:$PATH nice -n 19 m68k-atari-mint-gcc -O2 -s -o "$R/tests/mint/$p.prg" "$T/mint/$p.c"
done
[ -n "$MINT" ] || rm -rf "$O/mint"
echo "[ok] mintrun, MiNT test programs"
