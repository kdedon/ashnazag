#!/bin/sh
# build.sh -- the Voyage 200 environment's native host.
#
#   sh kernel/guest/voyage200/build.sh outdir
#
# Out: outdir/native.  Its module, tiplatform, is built with the other
# guest modules (guest/build.sh -m).
set -e
V=$(cd "$(dirname "$0")" && pwd)
G=$(cd "$V/.." && pwd)
AUX=$(cd "$G/../.." && pwd)
O=$1
[ -n "$O" ] || { echo "usage: build.sh outdir" >&2; exit 2; }
TC=$AUX/toolchain/amix
SYS=$TC/m68k-cbm-sysv4/sysroot
LIB=$SYS/usr/ccs/lib
NH=$G/nativehost
mkdir -p "$O/obj" "$O/extra"
O=$(cd "$O" && pwd)
CC="nice -n 19 $TC/bin/m68k-cbm-sysv4-gcc -O -Wall -Wno-comment -D__STDC__=0 -fno-builtin
	-I$V -I$G/include -I$NH -I$AUX/kernel/mac/display -I$AUX/kernel/mac/sound"
objs=
for c in "$V"/voyage200.c "$V"/hash.c "$NH"/nativehost.c; do
	$CC -c "$c" -o "$O/obj/$(basename "$c" .c).o"
	objs="$objs $O/obj/$(basename "$c" .c).o"
done
for s in "$V/calls.s"; do
	nice -n 19 "$TC/bin/m68k-cbm-sysv4-as" -o "$O/obj/$(basename "$s" .s).o" "$s"
	objs="$objs $O/obj/$(basename "$s" .s).o"
done
# libc's nonshared members
(cd "$O/extra" && ar x "$LIB/libc.so" && rm -f libc.so.1 && ar rc "$O/libextra.a" ./*.o)
LIBGCC=$(ls "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a | tail -1)
nice -n 19 "$TC/bin/m68k-cbm-sysv4-ld" -o "$O/native" "$LIB/crt1.o" "$LIB/crti.o" $objs \
	"$SYS/usr/lib/libc.so.1" "$O/libextra.a" "$LIBGCC" "$LIB/crtn.o"
echo "[ok] $O/native"
