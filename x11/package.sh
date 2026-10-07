#!/bin/sh
# package.sh -- disk-root tree for the X server and its manifest lines.
#
#   sh x11/package.sh
#
# In:  $X11W/src/xc (after build.sh).  Out: $X11W/pkg/ (the files) and
# $X11W/pkg/x11.manifest (lines for the disk root's manifest, sources
# relative to the manifest's directory, where pkg is linked as x11pkg).
set -e

D=$(cd "$(dirname "$0")" && pwd)
PLATFORM=${PLATFORM:-mac}
X11W=${X11W:-$D/../images/work/x11-$PLATFORM}
XC=$X11W/src/xc
P=$X11W/pkg
STRIP=$D/../toolchain/bin/m68k-elf-strip

MISC="5x7 5x8 6x9 6x10 6x12 6x13 6x13B 7x13 7x13B 7x14 7x14B 8x13 8x13B
9x15 9x15B 10x20 cursor"
DPI75="helvB08 helvB10 helvB12 helvB14 helvR08 helvR10 helvR12 helvR14"

rm -rf "$P"
mkdir -p "$P/bin" "$P/fonts/misc" "$P/fonts/75dpi" "$P/lib"
cp "$XC/programs/Xserver/Xamix" "$P/bin/Xamix"
"$STRIP" "$P/bin/Xamix"
for f in $MISC; do cp "$XC/fonts/bdf/misc/$f.bdf" "$P/fonts/misc/"; done
for f in $DPI75; do cp "$XC/fonts/bdf/75dpi/$f.bdf" "$P/fonts/75dpi/"; done
cp "$XC/fonts/bdf/misc/fonts.alias" "$P/fonts/misc/"
cp "$XC/programs/rgb/rgb.txt" "$P/lib/rgb.txt"
cp "$D/session/startx" "$D/session/xinitrc" "$P/lib/"
cp "$XC/programs/Xserver/Xext/SecurityPolicy" "$P/lib/SecurityPolicy"
# xdm and the session chooser (build.sh clients)
cp "$XC/programs/xdm/xdm" "$X11W/session/xchoose" "$X11W/session/xdmenv" "$P/bin/"
"$STRIP" "$P/bin/xdm" "$P/bin/xchoose" "$P/bin/xdmenv"
cp "$D/session/xsession" "$P/bin/"
mkdir -p "$P/xdm"
cp "$D"/session/xdm/* "$P/xdm/"
# the screen's login runs through xdmboot, which reads /etc/default/x
sed 's,^co:234:respawn:/etc/getty console console$,co:234:respawn:/usr/x11r6/lib/X11/xdm/xdmboot,' \
	"$D/../kernel/mac/diskroot/etc/inittab" > "$P/lib/inittab"
grep -q xdmboot "$P/lib/inittab"

# fonts.dir: "count" then "file XLFD", the XLFD from each BDF's FONT line
for dir in "$P/fonts/misc" "$P/fonts/75dpi"; do
	(cd "$dir" && for f in *.bdf; do
		printf '%s %s\n' "$f" "$(sed -n 's/^FONT //p' "$f" | head -1 | tr A-Z a-z)"
	done > fonts.tmp && { wc -l < fonts.tmp | tr -d ' '; cat fonts.tmp; } > fonts.dir &&
	rm fonts.tmp)
done

R=/usr/x11r6
{
	echo "# X11R6.3 server (Xamix) with its fonts and session files"
	echo "d $R 755 0 3"
	echo "d $R/bin 755 0 3"
	echo "f $R/bin/Xamix 755 0 3 x11pkg/bin/Xamix"
	echo "l $R/bin/X Xamix"
	echo "f $R/bin/startx 755 0 3 x11pkg/lib/startx"
	echo "l /usr/bin/startx $R/bin/startx"
	echo "f $R/bin/xdm 755 0 3 x11pkg/bin/xdm"
	echo "f $R/bin/xchoose 755 0 3 x11pkg/bin/xchoose"
	echo "f $R/bin/xdmenv 4755 0 3 x11pkg/bin/xdmenv"
	echo "f $R/bin/xsession 755 0 3 x11pkg/bin/xsession"
	echo "l /usr/bin/xsession $R/bin/xsession"
	echo "f /etc/default/x 644 0 3 x11pkg/xdm/default-x"
	echo "f /etc/inittab 644 0 3 x11pkg/lib/inittab"
	echo "d $R/lib 755 0 3"
	echo "d $R/lib/X11 755 0 3"
	echo "f $R/lib/X11/rgb.txt 444 0 3 x11pkg/lib/rgb.txt"
	echo "d $R/lib/X11/xserver 755 0 3"
	echo "f $R/lib/X11/xserver/SecurityPolicy 444 0 3 x11pkg/lib/SecurityPolicy"
	echo "d $R/lib/X11/xinit 755 0 3"
	echo "f $R/lib/X11/xinit/xinitrc 644 0 3 x11pkg/lib/xinitrc"
	echo "d $R/lib/X11/xdm 755 0 3"
	for f in xdm-config Xservers Xresources; do
		echo "f $R/lib/X11/xdm/$f 644 0 3 x11pkg/xdm/$f"
	done
	for f in Xstartup Xreset xdmboot; do
		echo "f $R/lib/X11/xdm/$f 755 0 3 x11pkg/xdm/$f"
	done
	echo "d $R/lib/X11/fonts 755 0 3"
	for sub in misc 75dpi; do
		echo "d $R/lib/X11/fonts/$sub 755 0 3"
		(cd "$P/fonts/$sub" && for f in *; do
			echo "f $R/lib/X11/fonts/$sub/$f 444 0 3 x11pkg/fonts/$sub/$f"
		done)
	done
} > "$P/x11.manifest"
du -sk "$P"

# the disk-root diff: tape segments 13 and 14, no Amiga X servers, the package
L=$D/../kernel/mac/diskroot
W=$X11W/diffwork
rm -rf "$W"; mkdir -p "$W/a" "$W/b"
cp "$L/root.manifest" "$L/mkdiskroot.sh" "$W/a/"
cp "$W/a/root.manifest" "$W/a/mkdiskroot.sh" "$W/b/"
sed -i 's/^for s in 02 03 07 10; do$/for s in 02 03 07 10 13 14; do/' "$W/b/mkdiskroot.sh"
grep -q '^for s in 02 03 07 10 13 14; do$' "$W/b/mkdiskroot.sh"
{
	echo
	echo "# X11: AMIX X11R4 clients (Xcore, Xbasic); the Amiga servers go"
	echo "a build/tape/13"
	echo "a build/tape/14"
	echo "r /usr/X/bin/X"
	echo "r /usr/X/bin/X2410"
	echo "r /usr/X/bin/Xdmi"
	echo "r /usr/X/bin/loadcoff"
	echo "r /usr/X/lib/tigagm.coff"
	echo "r /usr/lib/dmiexec"
	cat "$P/x11.manifest"
} >> "$W/b/root.manifest"
(cd "$W" && diff -u --label a/kernel/mac/diskroot/root.manifest \
	--label b/kernel/mac/diskroot/root.manifest a/root.manifest b/root.manifest;
 diff -u --label a/kernel/mac/diskroot/mkdiskroot.sh \
	--label b/kernel/mac/diskroot/mkdiskroot.sh a/mkdiskroot.sh b/mkdiskroot.sh) \
	> "$D/diskroot/x11-diskroot.diff" || true
rm -rf "$W"
echo "[OK] $D/diskroot/x11-diskroot.diff"
