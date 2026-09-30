#!/bin/sh
# mkmacimage.sh -- the X disk image plus the A/UX Mac environment.
#
#   sh images/macenv/mkmacimage.sh [kernel.elf [out.img]]
#
# AUXROOT (default: the path in tests/aux/auxroot) names the A/UX root,
# AUXROM the Mac ROM image for startmac; both proprietary, copied into
# the image only.  Other inputs as x11/mkimage.sh.
set -e
umask 077

D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
KERNEL=${1:-$AUX/kernel/build/unix-mac.elf}
X11W=${X11W:-$AUX/images/work/x11}
export X11W
OUT=${2:-$X11W/q800-mac.img}
P=${MACW:-$X11W/macpkg}
case $P in ""|/|"$HOME"|"$AUX") echo "[FAIL] MACW=$P"; exit 1 ;; esac
[ -n "$AUXROOT" ] || AUXROOT=$(cat "$AUX/tests/aux/auxroot")
AUXROM=${AUXROM:-$AUX/420DBFF3 - Quadra 700&900 & PB140&170.ROM}
TC=$AUX/toolchain/amix
SYS=$TC/m68k-cbm-sysv4/sysroot

# the A/UX copies go with the build tree, whatever the outcome
trap 'rm -rf "$P" "$X11W/auxtree"' EXIT
rm -rf "$P"
mkdir -p "$P"
sh "$AUX/kernel/guest/build.sh" -m "$KERNEL" "$P/guest" > "$P/guest.log" 2>&1 ||
	{ tail "$P/guest.log"; exit 1; }
mv "$P/guest/mod.d" "$P/mod.d"
for s in "$AUX"/kernel/dlm/libmod/*.s; do
	"$TC/bin/m68k-cbm-sysv4-as" -o "$P/lm_$(basename "$s" .s).o" "$s"
done
nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -w -D__STDC__=0 -I"$AUX/kernel/dlm/include" \
	-c "$AUX/kernel/mac/diskroot/pkg/auxreg.c" -o "$P/auxreg.o"
"$TC/bin/m68k-cbm-sysv4-ld" -o "$P/auxreg" "$SYS/usr/ccs/lib/crt1.o" \
	"$SYS/usr/ccs/lib/crti.o" "$P/auxreg.o" "$P"/lm_*.o "$SYS/usr/lib/libc.so.1" \
	"$SYS/usr/ccs/lib/crtn.o"
rm -rf "$P/guest" "$P"/*.o

cp "$D/S05aux" "$D/startmac" "$P/"
mv "$P/startmac" "$P/startmac.sh"
cp "$AUXROOT/mac/bin/startmac" "$AUXROOT/shlib/libc1_s" "$AUXROOT/shlib/libmac1_s" \
	"$AUXROOT/etc/fidd" "$AUXROOT/mac/lib/Patches/Patch.067C" \
	"$AUXROOT/mac/lib/SystemFiles/shared/Finder" "$AUXROOT/mac/sys/System Folder/System" "$P/"
cp "$AUXROM" "$P/rom"

MACPKG=$P sh "$AUX/x11/mkimage.sh" "$KERNEL" "$OUT"
