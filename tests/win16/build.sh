#!/bin/sh
# build.sh -- startwin for AMIX, on the display service (scr_fb.c).
#
#   sh tests/win16/build.sh outdir
#
# Out: outdir/startwin.  The runtime is first checked on this host
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
	nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O2 -w -D__STDC__=0 \
		-I"$G" -I"$AUX/kernel/guest/include" -I"$AUX/kernel/mac/display" -I"$AUX/kernel/mac/sound" $FTF \
		-c "$f" -o "$o"
	objs="$objs $o"
done
# sockets (WINSOCK, ws.c): libsocket and libnsl, as native clients link them
nice -n 19 "$TC/bin/m68k-cbm-sysv4-ld" -o "$OUT/startwin" "$SYS/usr/ccs/lib/crt1.o" \
	"$SYS/usr/ccs/lib/crti.o" $objs -L"$SYS/usr/lib" -lsocket -lnsl "$SYS/usr/lib/libc.so.1" \
	"$T/build/obj/libextra.a" $LIBM "$LIBGCC" "$SYS/usr/ccs/lib/crtn.o"
rm -rf "$OUT/obj"
echo "[ok] startwin for AMIX"
