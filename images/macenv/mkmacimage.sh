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
# The environment packages (images/envpkg/mkenvpkgs.sh and its inputs)
# are installed as pkgadd would; AMIGASTAGE adds the Amiga's Kickstart and
# SYS:, CUBASE (mkcubase.sh's) Cubase to guest's ~/TOS, V200ZIP (default
# media/v200.zip; V200ROM picks the dump) guest's ~/Voyage200.  Given the A/UX
# 2.0.1 CD (CD201) and the IIci ROM (AUXROM6), System 6 goes in too
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
# the environment packages, installed as pkgadd would
KERNEL=$KERNEL sh "$AUX/images/envpkg/mkenvpkgs.sh" "$P/envpkg" > "$P/envpkg.log" 2>&1 ||
	{ tail "$P/envpkg.log"; exit 1; }
ENVPKGS=
for p in envcore macenv tosenv amigaenv cpmenv smsqenv win16env v200env; do
	f=$P/envpkg/pkgs/env/$p-1.0.pkg
	[ ! -f "$f" ] || ENVPKGS="$ENVPKGS $f"
done
I=$P/image.manifest
G=$P/envpkg/guest/home/guest
# guest's folders, as maketos, startwin and startsmsq make them
for d in TOS WIN16 SMSQ; do
	[ ! -d "$G/$d" ] || printf 't\t/home/guest/%s\t100\t1\t%s\n' $d "$G/$d"
done > "$I"
# DRI's CP/M files; the user's Cubase in guest's ~/TOS
[ ! -d "$P/envpkg/dri/cpm" ] || printf 't\t/cpm\t0\t3\t%s\n' "$P/envpkg/dri/cpm" >> "$I"
CUBASE=${CUBASE:-$AUX/images/work/cubase/cubase.cpio}
[ ! -f "$CUBASE" ] || echo "a $CUBASE" >> "$I"
# the user's Voyage 200 Flash dump as guest's ~/Voyage200/media
V200ZIP=${V200ZIP:-$AUX/media/v200.zip}
if [ -f "$V200ZIP" ]; then
	mkdir -p "$P/v200"
	unzip -p "$V200ZIP" "${V200ROM:-voyage200v209.rom}" > "$P/v200/media"
	printf 't\t/home/guest/Voyage200\t100\t1\t%s\n' "$P/v200" >> "$I"
fi
# the datastreams, for pkgadd -d
for f in $ENVPKGS; do echo "f /var/spool/pkg/${f##*/} 644 0 3 $f"; done >> "$I"
if [ "$MACNETWORK" = 1 ] && [ -f "$CD81" ]; then
	sh "$D/network/build.sh" "$P/network"
	ENVPKGS="$ENVPKGS $P/envpkg/pkgs/env/macnet-1.0.pkg"
fi

# the Amiga environment's Kickstart, floppies and SYS: template from
# AMIGASTAGE (images/amigaenv/mkamiga.sh's output; proprietary, image only)
if [ -n "$AMIGASTAGE" ]; then
	S=$AMIGASTAGE/root
	echo "f /etc/amiga/kicka4000.rom 640 0 25 $S/etc/amiga/kicka4000.rom" >> "$I"
	for f in "$S"/amiga/media/*.adf "$S/amiga/media/manifest.json"; do
		echo "f ${f#$S} 640 0 25 $f"
	done >> "$I"
	for f in "$S"/amiga/rtg/Picasso96.*; do
		[ ! -f "$f" ] || echo "f ${f#$S} 644 0 3 $f"
	done >> "$I"
	cp -R "$S/amiga/sys" "$P/amigasys"
	chmod -R a+rX,go-w,u+w "$P/amigasys"
	# guest's ~/Amiga, as makeamiga makes it
	printf 't\t%s\t%s\t%s\t%s\n' /amiga/sys 0 3 "$P/amigasys" \
		/home/guest/Amiga 100 1 "$P/amigasys" >> "$I"
fi

cp "$D/motd" "$P/"
# A/UX's shared libraries, File ID daemon and, if given, ROM
cat >> "$I" <<EOF2
d /shlib 755 0 3
f /shlib/libc1_s 755 0 3 $AUXROOT/shlib/libc1_s
f /shlib/libmac1_s 755 0 3 $AUXROOT/shlib/libmac1_s
f /etc/aux/fidd 755 0 3 $AUXROOT/etc/fidd
EOF2
[ -z "$AUXROM" ] || echo "f /etc/aux/rom 444 0 3 $AUXROM" >> "$I"

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
# /usr and its directories keep the manifest's owners; guest's System
# Folder, as makemac makes it
for f in "$M"/* "$M"/.mac; do
	[ "$f" = "$M/usr" ] || printf 't\t/%s\t0\t3\t%s\n' "${f#$M/}" "$f"
done >> "$I"
printf 't\t/home/guest/System Folder\t100\t1\t%s\n' "$M/mac/lib/System Folder" >> "$I"
echo "f /usr/bin/systemfolder 755 0 3 $M/usr/bin/systemfolder" >> "$I"
echo "f /usr/lib/updtsysfldr 755 0 3 $M/usr/lib/updtsysfldr" >> "$I"

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
	find "$R6" -type d -exec chmod 755 {} +
	find "$R6" -type f -perm -u+x -exec chmod 755 {} +
	find "$R6" -type f ! -perm -u+x -exec chmod 644 {} +
	chmod 1777 "$R6/a201/tmp"
	chmod 444 "$R6/a201/etc/aux/rom"
	printf 't\t%s\t%s\t%s\t%s\n' /a201 0 3 "$R6/a201" \
		"/a201/home/guest/System Folder" 100 1 "$R6/a201/mac/sys/System Folder" >> "$I"
	cat >> "$I" <<'EOF2'
c /a201/dev/uinter0 660 0 25 54 0
c /a201/dev/console 622 0 3 0 0
EOF2
else
	echo "[warn] no System 6: needs $CD201 and $AUXROM6"
fi

# swap backs the Mac's memory (startmac's TBMEMORY, up to 32 MB)
# BOOTX=1 with the desktop database: mkdesktop.py logs in on the console,
# then sets xdm
bx=$BOOTX
[ "$DESKTOP" = 0 ] || bx=
ROOTMB=${ROOTMB:-320} SWAPMB=${SWAPMB:-96} MACPKG=$P ENVPKGS=$ENVPKGS BOOTX=$bx sh "$AUX/x11/mkimage.sh" "$KERNEL" "$OUT"
# the Finder's desktop database, made once in QEMU (DESKTOP=0: on the
# first startmac instead)
[ "$DESKTOP" = 0 ] || BOOTX=$BOOTX python3 "$D/mkdesktop.py" "$OUT" ||
	{ echo "[FAIL] desktop database (DESKTOP=0 leaves it to the first startmac)"; exit 1; }
