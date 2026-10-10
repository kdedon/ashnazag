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
# The TOS container (images/tosenv/mktos.sh) and, given DRI's release
# (CPMZIP), CP/M-68K (images/cpmenv/mkcpm.sh) are built in.  Given the
# A/UX 2.0.1 CD (CD201) and the IIci ROM (AUXROM6), so is System 6
# (startmac6).  Other inputs as x11/mkimage.sh.
set -e
umask 077

D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
KERNEL=${1:-$AUX/kernel/build/unix-mac.elf}
X11W=${X11W:-$AUX/images/work/x11-mac}
export X11W
OUT=${2:-$X11W/q800-mac.img}
P=${MACW:-$X11W/macpkg}
case $P in ""|/|"$HOME"|"$AUX") echo "[FAIL] MACW=$P"; exit 1 ;; esac
[ -n "$AUXROOT" ] || AUXROOT=$(cat "$AUX/tests/aux/auxroot")
CD761=${CD761:-$AUX/media/Mac OS 7.6.1.iso}
[ -f "$CD761" ] || { echo "[FAIL] no Mac OS 7.6.1 CD: $CD761"; exit 1; }
CD201=${CD201:-$AUX/media/AUX_2.0.1_CD_Image.iso}
AUXROM6=${AUXROM6:-$AUX/368CADFE - Mac IIci.ROM}
TC=$AUX/toolchain/amix
SYS=$TC/m68k-cbm-sysv4/sysroot
CD81=${CD81:-$AUX/media/MacOS8_1.iso}
MACNETWORK=${MACNETWORK:-1}
case $MACNETWORK in 0|1) ;; *) echo "[FAIL] MACNETWORK must be 0 or 1"; exit 1 ;; esac

# the A/UX copies go with the build tree, whatever the outcome
trap 'rm -rf "$P" "$X11W/auxtree"' EXIT
rm -rf "$P"
mkdir -p "$P"
sh "$AUX/kernel/guest/build.sh" -m "$KERNEL" "$P/guest" > "$P/guest.log" 2>&1 ||
	{ tail "$P/guest.log"; exit 1; }
mv "$P/guest/mod.d" "$P/mod.d"
if [ "$MACNETWORK" = 1 ] && [ -f "$CD81" ]; then
	sh "$D/network/build.sh" "$P/network"
	sh "$AUX/kernel/otbridge/build.sh" -k "$KERNEL" "$P/otbridge" > "$P/otbridge.log" 2>&1 ||
		{ tail "$P/otbridge.log"; exit 1; }
	[ -s "$P/otbridge/mod.d/otbridge" ] || { echo "[FAIL] missing otbridge module"; exit 1; }
	cp "$P/otbridge/mod.d/otbridge" "$P/mod.d/"
fi
for s in "$AUX"/kernel/dlm/libmod/*.s; do
	"$TC/bin/m68k-cbm-sysv4-as" -o "$P/lm_$(basename "$s" .s).o" "$s"
done
nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -w -D__STDC__=0 -I"$AUX/kernel/dlm/include" \
	-c "$AUX/kernel/mac/diskroot/pkg/auxreg.c" -o "$P/auxreg.o"
"$TC/bin/m68k-cbm-sysv4-ld" -o "$P/auxreg" "$SYS/usr/ccs/lib/crt1.o" \
	"$SYS/usr/ccs/lib/crti.o" "$P/auxreg.o" "$P"/lm_*.o "$SYS/usr/lib/libc.so.1" \
	"$SYS/usr/ccs/lib/crtn.o"
if [ -d "$P/network/Extensions" ]; then
	nice -n19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -w -D__STDC__=0 -I"$AUX/kernel/dlm/include" \
		-c "$D/network/otreg.c" -o "$P/otreg.o"
	"$TC/bin/m68k-cbm-sysv4-ld" -o "$P/otreg" "$SYS/usr/ccs/lib/crt1.o" \
		"$SYS/usr/ccs/lib/crti.o" "$P/otreg.o" "$P"/lm_*.o "$SYS/usr/lib/libc.so.1" \
		"$SYS/usr/ccs/lib/crtn.o"
	cat > "$P/network.manifest" <<'EOF'
f /usr/sbin/otreg 755 0 3 macpkg/otreg
f /usr/aux/lib/mod.d/otbridge 644 0 3 macpkg/mod.d/otbridge
c /dev/otbridge 660 0 25 55 0
c /dev/otbstation 660 0 25 55 1
c /dev/tcp 666 0 3 33 0
c /dev/udp 666 0 3 34 0
EOF
fi
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
nice -n 19 "$TC/bin/m68k-cbm-sysv4-gcc" -O -w -D__STDC__=0 -c "$D/mac6.c" -o "$P/mac6.o"
"$TC/bin/m68k-cbm-sysv4-ld" -o "$P/mac6" "$SYS/usr/ccs/lib/crt1.o" \
	"$SYS/usr/ccs/lib/crti.o" "$P/mac6.o" "$SYS/usr/lib/libc.so.1" \
	"$SYS/usr/ccs/lib/crtn.o"
rm -rf "$P/guest" "$P"/*.o
sh "$AUX/images/tosenv/mktos.sh" "$P/tos" ||
	{ echo "[FAIL] TOS container files"; exit 1; }
sh "$AUX/images/cpmenv/mkcpm.sh" "$P/cpm" > "$P/cpm.log" 2>&1 ||
	{ tail "$P/cpm.log"; echo "[FAIL] CP/M-68K environment files"; exit 1; }
sh "$AUX/images/winenv/mkwin.sh" "$P/win" > "$P/win.log" 2>&1 ||
	{ tail "$P/win.log"; echo "[FAIL] Win16 environment files"; exit 1; }

# the Amiga environment, installed as installmig would from AMIGASTAGE
# (images/amigaenv/mkamiga.sh's output; proprietary, image only).  The
# Kickstart ROM and the ADFs are for the display group only.
A=$P/amigapkg AP=$P/amigaprv
mkdir -p "$A" "$AP"
if [ -n "$AMIGASTAGE" ]; then
	S=$AMIGASTAGE/root
	mkdir -p "$A/usr/bin" "$A/usr/sbin" "$A/etc/amiga" "$A/amiga" "$AP/etc/amiga" "$AP/amiga/media"
	cp "$S/usr/bin/startmig" "$S/usr/bin/makeamiga" "$A/usr/bin/"
	cp "$S/usr/sbin/amigareg" "$A/usr/sbin/"
	cp "$S/etc/amiga/container-boot.rom" "$A/etc/amiga/"
	cp -R "$S/amiga/sys" "$A/amiga/"
	for d in rtg guest; do [ ! -d "$S/amiga/$d" ] || cp -R "$S/amiga/$d" "$A/amiga/"; done
	cp "$S/etc/amiga/kicka4000.rom" "$AP/etc/amiga/"
	cp "$S/amiga/media/"*.adf "$S/amiga/media/manifest.json" "$AP/amiga/media/"
	chmod -R a+rX,go-w "$A"
	chmod 755 "$A/usr/bin/startmig" "$A/usr/bin/makeamiga" "$A/usr/sbin/amigareg"
	chmod 750 "$AP/amiga/media"; chmod 640 "$AP/etc/amiga/kicka4000.rom" "$AP/amiga/media/"*
	# the archive carries its parents: they must not shut others out of /etc
	chmod 755 "$AP/etc" "$AP/etc/amiga" "$AP/amiga"
	# guest's ~/Amiga, as makeamiga makes it
	mkdir -p "$A/home/guest"
	cp -R "$A/amiga/sys" "$A/home/guest/Amiga"
	chmod -R u+w "$A/home/guest/Amiga"
fi
chmod 755 "$A" "$AP"
(cd "$A" && find . -depth -print | grep -a -v -e '^\./home' |
	cpio -o -H newc -R 0:3 --quiet) > "$P/amiga.cpio"
(cd "$A" && find home/guest/Amiga -print 2> /dev/null |
	cpio -o -H newc -R 100:1 --quiet) > "$P/amigaguest.cpio"
(cd "$AP" && find . -depth -print | cpio -o -H newc -R 0:25 --quiet) > "$P/amigaprv.cpio"
rm -rf "$A" "$AP"

cp "$D/S05aux" "$D/startmac" "$D/makemac" "$D/startmac6" "$D/motd" "$P/"
mv "$P/startmac" "$P/startmac.sh"
cp "$AUXROOT/shlib/libc1_s" "$AUXROOT/shlib/libmac1_s" "$AUXROOT/etc/fidd" "$P/"
[ -z "$AUXROM" ] || cp "$AUXROM" "$P/rom"

# A/UX's Mac files as A/UX installs them, less its per-directory File
# Manager caches; owned by root, group sys
M=$P/macroot
mkdir -p "$M/usr/bin" "$M/usr/lib" "$M/.mac/localhost/Desktop Folder" "$M/.mac/localhost/Trash"
(cd "$AUXROOT" && tar cf - mac " Applications" " Documentation" " Shared Data" \
	" System Folder alias") | (cd "$M" && tar xf -)
COFFRSRC="$AUXROOT/mac/bin/%CommandShell" sh "$AUX/tests/aux/coffcarrier/build.sh" "$P/coffcarrier"
python3 "$AUX/tests/aux/coffcarrier/install.py" "$AUXROOT/mac/bin/CommandShell" \
    "$AUXROOT/mac/bin/%CommandShell" "$P/coffcarrier/COFFCarrier" "$M"
rm -rf "$P/coffcarrier"
cp "$AUXROOT/usr/bin/systemfolder" "$M/usr/bin/"
cp "$AUXROOT/usr/lib/updtsysfldr" "$M/usr/lib/"
find "$M" -name '.fs_*' -o -name '%.fs_*' | while read f; do rm -f "$f"; done
# Shut Down's dialog (root's only with other users logged in): Cancel ends
# the session, so it reads Logout
python3 -c "import sys; sys.path.insert(0, sys.argv[1]); import rsrcedit
f = sys.argv[2]; d = bytearray(open(f, 'rb').read())
for i in (128, 129, 130):
	o, l, a = rsrcedit.resource(d, b'DITL', i)
	k = d.index(b'\x04\x06Cancel', o, o + l)
	d[k + 2:k + 8] = b'Logout'
open(f, 'wb').write(d)" "$AUX/tools" "$M/mac/lib/Resources/%AUX Resources"
# Mac OS 7.6.1 in place of A/UX's System 7.0.1: the template, and
# root's copy
rm -rf "$M/mac/sys/System Folder"
sh "$D/mksys76.sh" "$CD761" "$M/mac/lib/System Folder" "$AUXROOT"
cp -rp "$M/mac/lib/System Folder" "$M/mac/sys/"
# users' Special menu: Log Out in place of Restart and Shut Down
python3 "$D/userfinder.py" "$M/mac/lib/System Folder/%Finder"
# Mac OS 8.1, environment 8.1's template (makemac -e 8.1)
CD81=${CD81:-$AUX/media/MacOS8_1.iso}
if [ -f "$CD81" ]; then
	S81="$M/mac/lib/env/8.1/System Folder"
	FULL=1 sh "$D/mksys81.sh" "$CD81" "$S81" "$AUXROOT"
	[ ! -d "$P/network/Extensions" ] || cp -rp "$P/network/Extensions/." "$S81/Extensions/"
	cp "$S81/Finder" "$S81/../RootFinder"
	cp "$S81/%Finder" "$S81/../%RootFinder"
	python3 "$D/userfinder.py" "$S81/%Finder"
	(cd "$S81" && find . -type f ! -name .stamp -print0 | LC_ALL=C sort -z | xargs -0 sha256sum) |
		sha256sum | cut -c1-16 > "$S81/.stamp"
fi
find "$M" -type d -exec chmod 755 {} +
chmod 700 "$M/.mac"
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

# System 6 from the A/UX 2.0.1 CD (CD201), when present: A/UX 2's root
# at /a201 with its own ROM, the IIci's (AUXROM6), and guest's System
# Folder there
R6=$P/a201
if [ -f "$CD201" ] && [ -f "$AUXROM6" ]; then
	sh "$D/mksys6.sh" "$CD201" "$R6/a201"
	mkdir -p "$R6/a201/dev" "$R6/a201/etc/aux" "$R6/a201/home/guest"
	cp "$AUXROM6" "$R6/a201/etc/aux/rom"
	echo "guest::100:1:guest:/home/guest:/bin/sh" >> "$R6/a201/etc/passwd"
	: > "$R6/a201/mac/sys/System Folder/.stamp"
	cp -rp "$R6/a201/mac/sys/System Folder" "$R6/a201/home/guest/"
	find "$R6" -type d -exec chmod 755 {} +
	find "$R6" -type f -perm -u+x -exec chmod 755 {} +
	find "$R6" -type f ! -perm -u+x -exec chmod 644 {} +
	chmod 1777 "$R6/a201/tmp"
	chmod 444 "$R6/a201/etc/aux/rom"
	(cd "$R6" && find . -depth -print | grep -a -v -e '^\./a201/home/guest/' |
		cpio -o -H newc -R 0:3 --quiet) > "$P/a201.cpio"
	(cd "$R6" && find "a201/home/guest/System Folder" -print |
		cpio -o -H newc -R 100:1 --quiet) > "$P/a201guest.cpio"
else
	echo "[warn] no System 6: needs $CD201 and $AUXROM6"
fi
rm -rf "$R6"

# swap backs the Mac's memory (startmac's TBMEMORY, up to 32 MB)
# BOOTX=1 with the desktop database: mkdesktop.py logs in on the console,
# then sets xdm
bx=$BOOTX
[ "$DESKTOP" = 0 ] || bx=
ROOTMB=${ROOTMB:-320} SWAPMB=${SWAPMB:-96} MACPKG=$P BOOTX=$bx sh "$AUX/x11/mkimage.sh" "$KERNEL" "$OUT"
# the Finder's desktop database, made once in QEMU (DESKTOP=0: on the
# first startmac instead)
[ "$DESKTOP" = 0 ] || BOOTX=$BOOTX python3 "$D/mkdesktop.py" "$OUT" ||
	{ echo "[FAIL] desktop database (DESKTOP=0 leaves it to the first startmac)"; exit 1; }
