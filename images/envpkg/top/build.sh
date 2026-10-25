#!/bin/sh
# build.sh outdir -- top and libkstat: outdir/top, outdir/libkstat.a.
# Fails on a 64-bit multiply or divide (68060) or any FPU instruction.
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
KS=$AUX/kernel/kstat
O=$1
[ -n "$O" ] || { echo "usage: build.sh outdir" >&2; exit 2; }
TC=$AUX/toolchain/amix
SYS=$TC/m68k-cbm-sysv4/sysroot
PATH="$AUX/toolchain/linux/bin:$PATH"
CC="nice -n 19 $TC/bin/m68k-cbm-sysv4-gcc -O -w -D__STDC__=0 -DSVR4 -I$O -I$D -I$KS"
mkdir -p "$O/obj"
# kill's signal names, from the system's numbers
{ echo 'struct sigdesc { char *name; int number; } sigdesc[] = {'
  sed -n 's/^#define[	 ]*SIG\([A-Z][A-Z0-9]*\)[	 ][	 ]*\([1-9][0-9]\{0,1\}\)\([^0-9x].*\)\{0,1\}$/	{ "\1", \2 },/p' \
	"$SYS/usr/include/sys/signal.h"
  echo '	{ 0, 0 } };'; } > "$O/sigdesc.h"
n=`sed -n 's/.*", \([0-9]*\) },/\1/p' "$O/sigdesc.h" | sort -n | tail -1`
echo "#define NSIG $((n + 1))" >> "$O/sigdesc.h"
$CC -c "$KS/libkstat.c" -o "$O/obj/libkstat.o"
rm -f "$O/libkstat.a"
m68k-linux-gnu-ar rc "$O/libkstat.a" "$O/obj/libkstat.o"
objs=
for c in top commands display screen username utils version m_ashnazag snprintf; do
	$CC -m68000 -c "$D/$c.c" -o "$O/obj/$c.o"
	objs="$objs $O/obj/$c.o"
done
$CC -c "$D/divsi3.c" -o "$O/obj/divsi3.o"
objs="$objs $O/obj/divsi3.o"
"$TC/bin/m68k-cbm-sysv4-ld" -o "$O/top" "$SYS/usr/ccs/lib/crt1.o" "$SYS/usr/ccs/lib/crti.o" \
	$objs "$O/libkstat.a" -L"$SYS/usr/ccs/lib" -ltermcap "$SYS/usr/lib/libc.so.1" \
	"$SYS/usr/ccs/lib/libc.so" `ls "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a | tail -1` \
	"$SYS/usr/ccs/lib/crtn.o"
python3 "$AUX/kernel/atari/chk64.py" $objs "$O/obj/libkstat.o"
if m68k-linux-gnu-objdump -d $objs "$O/obj/libkstat.o" | grep -E '	f[a-z]+(\.[bwlsdxp])?	' | grep .; then
	echo "[FAIL] FPU instructions"; exit 1
fi
