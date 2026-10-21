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
# media/v200.zip; V200ROM picks the dump) guest's ~/Voyage200, LISAMEDIA
# guest's ~/Lisa, NEXTCD guest's ~/NeXT/openstep, WINMEDIA (default media/) Windows for Workgroups 3.11
# (WFW), Windows 3.11 (WIN311) and Wabi in guest's ~/WIN16, QUAKE, GLQUAKE and QUAKEPAK /usr/games.  Given the A/UX
# 2.0.1 CD (CD201) and the IIci ROM (AUXROM6), System 6 goes in too
# (startmac -e 6).  Other inputs as x11/mkimage.sh.
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
for p in envcore macenv tosenv amigaenv cpmenv win16env lisaenv v200env nextenv; do
	f=`echo "$P/envpkg/pkgs/env/$p"-*.pkg`
	[ ! -f "$f" ] || ENVPKGS="$ENVPKGS $f"
done
I=$P/image.manifest
G=$P/envpkg/guest/home/guest
# the user's Windows for Workgroups 3.11 (WFW) and Windows 3.11 (WIN311)
# disks and Wabi 2.2 (WINMEDIA) in guest's ~/WIN16 and ~/WIN16/win311, as
# startwin -install puts them, with a startwin built for this host
WINMEDIA=${WINMEDIA:-$AUX/media}
[ -n "$WFW" ] || for WFW in "$WINMEDIA"/*[Ww]orkgroups*3.11*.7z; do break; done
[ -n "$WIN311" ] || for WIN311 in "$WINMEDIA"/*Windows*3.11*[Rr]etail*.7z; do break; done
WABI=$WINMEDIA/Wabi/x86/pkgs/SUNWwabi
SW=$P/sw/startwin
WINS=
# winst archive dir: Windows from archive's disk images into dir, C: as
# the guest sees it
winst() {
	rm -rf "$P/wd"
	7z e -y -o"$P/wd" "$1" > /dev/null &&
	"$SW" -C "$G/WIN16$2" -install "$P"/wd/[Dd][Ii][Ss][Kk]*.[Ii][Mm][Gg] "$WABI" > "$P/wfw.log" ||
		{ cat "$P/wfw.log"; exit 1; }
	rm -rf "$P/wd"
	sed -i "s|^Drives.C=$G/WIN16$2|Drives.C=/home/guest/WIN16$2|" "$G/WIN16$2/windows/wabi.ini"
}
if [ ! -d "$G/WIN16" ] || [ ! -f "$WABI/pkgmap" ] || { [ ! -f "$WFW" ] && [ ! -f "$WIN311" ]; }; then
	echo "[skip] Win16: no Windows disks or Wabi in $WINMEDIA"
elif ! sh "$AUX/tests/win16/host.sh" "$P/sw" > "$P/sw.log" 2>&1; then
	echo "[skip] Win16: no startwin for this host: $(tail -1 "$P/sw.log")"
else
	if [ -f "$WFW" ]; then
		winst "$WFW" ""
		WINS=wfw
		echo "[ok] Win16: Windows for Workgroups 3.11 and Wabi"
	fi
	if [ -f "$WIN311" ]; then
		winst "$WIN311" /win311
		printf 'id=win311\nname=win311\ncreated=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
			> "$G/WIN16/win311/.env"
		WINS="$WINS win311"
		echo "[ok] Win16: Windows 3.11 and Wabi in ~/WIN16/win311"
	fi
	# no build paths left in the guest's settings
	if grep -rl "$P" "$G/WIN16" > "$P/wfw.log"; then
		echo "[FAIL] Win16: build paths in $(cat "$P/wfw.log")"
		exit 1
	fi
fi
rm -rf "$P/sw"
# guest's folders, as starttos and startwin make them
for d in TOS WIN16; do
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
	echo "l /home/guest/Voyage200/native /usr/lib/voyage200/native" >> "$I"
fi
# the user's Lisa ROM and ProFile image (LISAMEDIA, default
# media/lisa/imported: H.rom, profile.dc42) as guest's ~/Lisa
LISAMEDIA=${LISAMEDIA:-$AUX/media/lisa/imported}
if [ -f "$LISAMEDIA/H.rom" ] && [ -f "$LISAMEDIA/profile.dc42" ]; then
	mkdir -p "$P/lisa"
	cp "$LISAMEDIA/H.rom" "$P/lisa/lisa.rom"
	cp "$LISAMEDIA/profile.dc42" "$P/lisa/"
	printf 't\t/home/guest/Lisa\t100\t1\t%s\n' "$P/lisa" >> "$I"
	echo "l /home/guest/Lisa/native /usr/lib/lisa/native" >> "$I"
	echo "l /home/guest/Lisa/media profile.dc42" >> "$I"
fi
# the user's OPENSTEP 4.2 CD (NEXTCD, default media/openstep's) as
# guest's ~/NeXT/openstep, as makenext.sh installs it
NEXTCD=${NEXTCD:-$AUX/media/openstep/Openstep-4.2-Intel-User.iso}
if [ -f "$NEXTCD" ]; then
	HOME=$P/next sh "$AUX/tools/makenext.sh" openstep "$NEXTCD" > "$P/next.log" 2>&1 ||
		{ tail "$P/next.log"; exit 1; }
	printf 't\t/home/guest/NeXT\t100\t1\t%s\n' "$P/next/NeXT" >> "$I"
	echo "l /home/guest/NeXT/openstep/native /usr/lib/next/native" >> "$I"
fi
# Quake (gl/quake/build.sh's quake and glquake, QUAKE and GLQUAKE) and
# the user's id1/pak0.pak (QUAKEPAK) in /usr/games
for f in "$QUAKE" "$GLQUAKE"; do
	[ -z "$f" ] || echo "f /usr/games/${f##*/} 755 2 2 $f" >> "$I"
done
[ -z "$QUAKEPAK" ] || echo "f /usr/games/id1/pak0.pak 444 2 2 $QUAKEPAK" >> "$I"
# the datastreams, for pkgadd -d
for f in $ENVPKGS; do echo "f /var/spool/pkg/${f##*/} 644 0 3 $f"; done >> "$I"
if [ "$MACNETWORK" = 1 ] && [ -f "$CD81" ]; then
	sh "$D/network/build.sh" "$P/network"
	ENVPKGS="$ENVPKGS `echo "$P"/envpkg/pkgs/env/macnet-*.pkg`"
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
	sh "$AUX/images/amigaenv/refresh-sys.sh" "$P/amigasys" || exit 1
	chmod -R a+rX,go-w,u+w "$P/amigasys"
	# guest's ~/Amiga, as startamiga makes it
	printf 't\t%s\t%s\t%s\t%s\n' /amiga/sys 0 3 "$P/amigasys" \
		/home/guest/Amiga 100 1 "$P/amigasys" >> "$I"
fi

# the motd names the Windows installed
case $WINS in
*wfw*win311*) cp "$D/motd" "$P/" ;;
*wfw*) grep -v 'Windows 3.11:' "$D/motd" > "$P/motd" ;;
*win311*) sed -e '/Windows for Workgroups/d' \
	-e 's/^        Windows 3.11:  startwin/ Win16  startwin/' "$D/motd" > "$P/motd" ;;
*) grep -v 'startwin' "$D/motd" > "$P/motd" ;;
esac
# A/UX's shared libraries, File ID daemon and, if given, ROM
cat >> "$I" <<EOF2
d /shlib 755 0 3
f /shlib/libc1_s 755 0 3 $AUXROOT/shlib/libc1_s
f /shlib/libmac1_s 755 0 3 $AUXROOT/shlib/libmac1_s
f /etc/aux/fidd 755 0 3 $AUXROOT/etc/fidd
EOF2
[ -z "$AUXROM" ] || echo "f /etc/aux/rom 444 0 3 $AUXROM" >> "$I"

# A/UX's Mac files with 7.6.1, owned by root, group sys
M=$P/macroot
sh "$D/mkmacroot.sh" "$AUXROOT" "$CD761" "$M"
# Mac OS 8.1, environment 8.1's template
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
# Folder, as startmac makes it
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

# the written image must boot to the login prompt from the Quadra ROM
sh "$D/romboot.sh" "$OUT" || exit 1
