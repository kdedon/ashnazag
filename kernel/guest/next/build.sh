#!/bin/sh
# build.sh -- the NeXT environment's native host.
#
#   sh kernel/guest/next/build.sh outdir
#
# Out: outdir/native.  Its module, nextguest, is built with the other
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
# the guest's filesystem routines, linked for their region, as data for the host
HFS_CODE=$(sed -n 's/^#define HF_BASE *\(0x[0-9a-f]*\)UL.*/\1/p' "$V/hfswire.h")
for c in hfsguest sndguest; do
	$TC/bin/m68k-cbm-sysv4-gcc -O -Wall -Wno-implicit -fno-builtin -m68040 -I"$V" -c "$V/$c.c" -o "$O/obj/$c.o"
done
$TC/bin/m68k-cbm-sysv4-ld -o "$O/hfsguest.elf" -Ttext=$(printf '0x%x' $((HFS_CODE + 0x8000))) -e hf_init "$O/obj/hfsguest.o" "$O/obj/sndguest.o"
$TC/bin/m68k-cbm-sysv4-objcopy -O binary "$O/hfsguest.elf" "$O/hfsguest.bin"
ENTRY=$($TC/bin/m68k-cbm-sysv4-nm "$O/hfsguest.elf" | sed -n 's/^\([0-9a-f]*\) T hf_init$/\1/p')
{
	echo 'unsigned char hfs_blob[] = {'
	od -An -v -tu1 "$O/hfsguest.bin" | sed 's/  *\([0-9]*\)/\1,/g'
	echo '0};'
	echo "unsigned long hfs_blob_len = $(wc -c < "$O/hfsguest.bin");"
	echo "unsigned long hfs_entry = $((0x$ENTRY - HFS_CODE - 0x8000));"
} > "$O/hfsblob.c"
for c in "$V"/next.c "$V"/hfshost.c "$O"/hfsblob.c "$NH"/nativehost.c "$G"/sndout.c; do
	$CC -c "$c" -o "$O/obj/$(basename "$c" .c).o"
	objs="$objs $O/obj/$(basename "$c" .c).o"
done
# libc's nonshared members
(cd "$O/extra" && ar x "$LIB/libc.so" && rm -f libc.so.1 && ar rc "$O/libextra.a" ./*.o)
LIBGCC=$(ls "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a | tail -1)
# linked above the guest's space
HOST=$(sed -n 's/^#define NX_HOST *\(0x[0-9a-f]*\)UL.*/\1/p' "$V/profile.h")
"$TC/bin/m68k-cbm-sysv4-ld" --verbose | sed -e '1,/^=====/d' -e '/^=====/,$d' -e "s/0x80000000/$HOST/" > "$O/native.ld"
nice -n 19 "$TC/bin/m68k-cbm-sysv4-ld" -T "$O/native.ld" -o "$O/native" "$LIB/crt1.o" "$LIB/crti.o" $objs \
	"$SYS/usr/lib/libc.so.1" "$O/libextra.a" "$LIBGCC" "$LIB/crtn.o"
echo "[ok] $O/native"
