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
# and the ports (images/ports) are installed as pkgadd would; AMIGASTAGE adds the Amiga's Kickstart and
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
for p in envcore macenv tosenv amigaenv cpmenv win16env lisaenv v200env nextenv sdhcp kstat top; do
	f=`echo "$P/envpkg/pkgs/env/$p"-*.pkg`
	[ ! -f "$f" ] || ENVPKGS="$ENVPKGS $f"
done
ENVPKGS="$ENVPKGS `sh "$AUX/images/ports/cached.sh"`"
I=$P/image.manifest
G=$P/envpkg/guest/home/guest
. "$AUX/images/envpkg/envmedia.sh"
GH=/home/guest IH=$I
envwin16
# guest's folders, as starttos and startwin make them
for d in TOS WIN16; do
	[ ! -d "$G/$d" ] || printf 't\t/home/guest/%s\t100\t1\t%s\n' $d "$G/$d"
done > "$I"
# DRI's CP/M files; the user's Cubase in guest's ~/TOS
[ ! -d "$P/envpkg/dri/cpm" ] || printf 't\t/cpm\t0\t3\t%s\n' "$P/envpkg/dri/cpm" >> "$I"
CUBASE=${CUBASE:-$AUX/images/work/cubase/cubase.cpio}
[ ! -f "$CUBASE" ] || echo "a $CUBASE" >> "$I"
envv200
envlisa
envnext
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

envamiga

# the motd names the Windows installed
case $WINS in
*wfw*win311*) cp "$D/motd" "$P/" ;;
*wfw*) grep -v 'Windows 3.11:' "$D/motd" > "$P/motd" ;;
*win311*) sed -e '/Windows for Workgroups/d' \
	-e 's/^        Windows 3.11:  startwin/ Win16  startwin/' "$D/motd" > "$P/motd" ;;
*) grep -v 'startwin' "$D/motd" > "$P/motd" ;;
esac
SYS6=1 envmac

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
