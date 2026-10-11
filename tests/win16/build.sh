#!/bin/sh
# build.sh -- startwin for AMIX, on the display service (scr_fb.c).
#
#   sh tests/win16/build.sh outdir
#
# Out: outdir/startwin, outdir/x86diff.  The runtime is first checked on this host
# (tests/win16/run.sh).  Nothing from Windows is needed or built in;
# FreeType's TrueType parts are (fetched by ft/freetype.sh).
set -e
T=$(cd "$(dirname "$0")/.." && pwd)
AUX=$(cd "$T/.." && pwd)
G=$AUX/kernel/guest/win16
OUT=$1
TC=$AUX/toolchain/amix
SYS=$TC/m68k-cbm-sysv4/sysroot
case $OUT in "") echo "usage: build.sh outdir" >&2; exit 2 ;; esac
mkdir -p "$OUT/obj"
sh "$T/win16/run.sh"
LIBGCC=$(ls "$TC"/lib/gcc-lib/m68k-cbm-sysv4/*/libgcc.a | tail -1)
LIBM=; for l in "$SYS/usr/lib/libm.a" "$SYS/usr/ccs/lib/libm.a"; do [ -f "$l" ] && LIBM=$l; done
objs=
# FreeType for TrueType (kernel/guest/win16/ft/freetype.sh), then ours
FTF=$(sh "$G/ft/freetype.sh" flags)
for f in $(sh "$G/ft/freetype.sh" srcs) "$G"/*.c "$AUX/kernel/guest/sndout.c"; do
	case $(basename "$f") in scr_null.c|snd_null.c) continue ;; esac
	o=$OUT/obj/$(basename "$f" .c).o
	nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O2 -fomit-frame-pointer -w -D__STDC__=0 -DX86ASM \
		-I"$G" -I"$AUX/kernel/guest/include" -I"$AUX/kernel/mac/display" -I"$AUX/kernel/mac/sound" $FTF \
		-c "$f" -o "$o"
	objs="$objs $o"
done
# the run loop in 68k assembly (its labels kept, for PC profiles) and its
# check against the C core
AS="nice -n 19 $TC/bin/m68k-cbm-sysv4-as -L -I$AUX/kernel/guest"
$AS -o "$OUT/obj/x86a.o" "$G/x86a.s"
$AS -o "$OUT/obj/x86com.o" "$AUX/kernel/guest/x86com/x86com.s"
objs="$objs $OUT/obj/x86a.o $OUT/obj/x86com.o"
nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O2 -w -D__STDC__=0 -I"$G" -c "$T/win16/x86diff.c" -o "$OUT/obj/x86diff.o"
nice -n 19 "$TC/bin/m68k-cbm-sysv4-ld" -o "$OUT/x86diff" "$SYS/usr/ccs/lib/crt1.o" "$SYS/usr/ccs/lib/crti.o" \
	"$OUT/obj/x86diff.o" "$OUT/obj/x86.o" "$OUT/obj/x87.o" "$OUT/obj/x86a.o" "$OUT/obj/x86com.o" \
	$LIBM "$SYS/usr/lib/libc.so.1" "$T/build/obj/libextra.a" "$LIBGCC" "$SYS/usr/ccs/lib/crtn.o"
# sockets (WINSOCK, ws.c): libsocket and libnsl, as native clients link them
nice -n 19 "$TC/bin/m68k-cbm-sysv4-ld" -o "$OUT/startwin" "$SYS/usr/ccs/lib/crt1.o" \
	"$SYS/usr/ccs/lib/crti.o" $objs -L"$SYS/usr/lib" -lsocket -lnsl $LIBM "$SYS/usr/lib/libc.so.1" \
	"$T/build/obj/libextra.a" "$LIBGCC" "$SYS/usr/ccs/lib/crtn.o"
rm -rf "$OUT/obj"
echo "[ok] startwin for AMIX"
