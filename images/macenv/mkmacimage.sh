#!/bin/sh
# mkmacimage.sh -- the X disk image plus the A/UX Mac environment.
#
#   sh images/macenv/mkmacimage.sh [kernel.elf [out.img]]
#
# AUXROOT (default: the path in tests/aux/auxroot) names the A/UX root,
# whose Mac files go in as A/UX installs them; proprietary, copied into
# the image only.  A Mac uses its own ROM; AUXROM, if set, names a ROM
# image installed as /etc/aux/rom for hosts without one.  The
# desktop database is made in QEMU (mkdesktop.py, the Quadra 800 ROM).
# The TOS container (images/tosenv/mktos.sh) is built in.  Other inputs as x11/mkimage.sh.
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
CD761=${CD761:-$AUX/media/Mac OS 7.6.1.iso}
[ -f "$CD761" ] || { echo "[FAIL] no Mac OS 7.6.1 CD: $CD761"; exit 1; }
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
nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -w -D__STDC__=0 -c "$D/macdiag.c" -o "$P/macdiag.o"
"$TC/bin/m68k-cbm-sysv4-ld" -o "$P/macdiag" "$SYS/usr/ccs/lib/crt1.o" \
	"$SYS/usr/ccs/lib/crti.o" "$P/macdiag.o" "$P"/lm_*.o "$SYS/usr/lib/libc.so.1" \
	"$SYS/usr/ccs/lib/crtn.o"
nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -w -D__STDC__=0 -I"$AUX/kernel/mac/display" \
	-c "$D/macscrn.c" -o "$P/macscrn.o"
"$TC/bin/m68k-cbm-sysv4-ld" -o "$P/macscrn" "$SYS/usr/ccs/lib/crt1.o" \
	"$SYS/usr/ccs/lib/crti.o" "$P/macscrn.o" "$P"/lm_*.o "$SYS/usr/lib/libc.so.1" \
	"$SYS/usr/ccs/lib/crtn.o"
nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -w -D__STDC__=0 -c "$D/envlock.c" -o "$P/envlock.o"
"$TC/bin/m68k-cbm-sysv4-ld" -o "$P/envlock" "$SYS/usr/ccs/lib/crt1.o" \
	"$SYS/usr/ccs/lib/crti.o" "$P/envlock.o" "$SYS/usr/lib/libc.so.1" \
	"$SYS/usr/ccs/lib/crtn.o"
rm -rf "$P/guest" "$P"/*.o
sh "$AUX/images/tosenv/mktos.sh" "$P/tos" ||
	{ echo "[FAIL] TOS container files"; exit 1; }

cp "$D/S05aux" "$D/startmac" "$D/makemac" "$P/"
mv "$P/startmac" "$P/startmac.sh"
cp "$AUXROOT/shlib/libc1_s" "$AUXROOT/shlib/libmac1_s" "$AUXROOT/etc/fidd" "$P/"
[ -z "$AUXROM" ] || cp "$AUXROM" "$P/rom"

# A/UX's Mac files as A/UX installs them, less its per-directory File
# Manager caches; owned by root, group sys
M=$P/macroot
mkdir -p "$M/usr/bin" "$M/usr/lib" "$M/.mac/localhost/Desktop Folder" "$M/.mac/localhost/Trash"
(cd "$AUXROOT" && tar cf - mac " Applications" " Documentation" " Shared Data" \
	" System Folder alias") | (cd "$M" && tar xf -)
cp "$AUXROOT/usr/bin/systemfolder" "$M/usr/bin/"
cp "$AUXROOT/usr/lib/updtsysfldr" "$M/usr/lib/"
find "$M" -name '.fs_*' -o -name '%.fs_*' | while read f; do rm -f "$f"; done
# a user's Shut Down and Restart: Cancel ends the session, so it reads Logout
python3 -c "import sys; sys.path.insert(0, sys.argv[1]); import rsrcedit
f = sys.argv[2]; d = bytearray(open(f, 'rb').read())
for i in (129, 130):
	o, l, a = rsrcedit.resource(d, b'DITL', i)
	k = d.index(b'\x04\x06Cancel', o, o + l)
	d[k + 2:k + 8] = b'Logout'
open(f, 'wb').write(d)" "$AUX/tools" "$M/mac/lib/Resources/%AUX Resources"
# Mac OS 7.6.1 in place of A/UX's System 7.0.1: the template, and
# root's copy
rm -rf "$M/mac/sys/System Folder"
sh "$D/mksys76.sh" "$CD761" "$M/mac/lib/System Folder" "$AUXROOT"
cp -rp "$M/mac/lib/System Folder" "$M/mac/sys/"
find "$M" -type d -exec chmod 755 {} +
find "$M" -type f -perm -u+x -exec chmod 755 {} +
find "$M" -type f ! -perm -u+x -exec chmod 644 {} +
# /usr and its directories keep the manifest's owners
(cd "$M" && find . -depth -print | grep -a -v -x -e ./usr -e ./usr/bin -e ./usr/lib |
	cpio -o -H newc -R 0:3 --quiet) > "$P/mac.cpio"
# guest's System Folder, as makemac makes it
mkdir -p "$M/home/guest"
cp -rp "$M/mac/lib/System Folder" "$M/home/guest/"
(cd "$M" && find "home/guest/System Folder" | cpio -o -H newc -R 100:1 --quiet) > "$P/macguest.cpio"
rm -rf "$M"

# swap backs the Mac's memory (startmac's TBMEMORY, up to 32 MB)
ROOTMB=${ROOTMB:-224} SWAPMB=${SWAPMB:-96} MACPKG=$P sh "$AUX/x11/mkimage.sh" "$KERNEL" "$OUT"
# the Finder's desktop database, made once in QEMU (DESKTOP=0: on the
# first startmac instead)
[ "$DESKTOP" = 0 ] || python3 "$D/mkdesktop.py" "$OUT" ||
	{ echo "[FAIL] desktop database (DESKTOP=0 leaves it to the first startmac)"; exit 1; }
