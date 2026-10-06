#!/bin/sh
# build.sh -- cross-build XView 3.2p1.4 (libxview, libolgx, olwm, olvwm,
# cmdtool, textedit, clock, props) against the X11R6.3 client libraries.
#
#   sh x11/xview/build.sh
#
# Needs x11/build.sh's tree, imake and clibs stages in $X11W.  Source:
# images/vendor.manifest (xview, xview-sp1).  Out: $X11W/xview/dist
# (/usr/openwin) and $X11W/xview/XVIEW.pkg.
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
PLATFORM=${PLATFORM:-mac}
X11W=${X11W:-$AUX/images/work/x11-$PLATFORM}
XC=$X11W/src/xc
W=$X11W/xview
XV=$W/xview-3.2p1.4
SYS=$AUX/toolchain/amix/m68k-cbm-sysv4/sysroot
MAKE="nice -n 19 make -j1"
CLIENTS="clock olwm olwmslave cmdtool textedit props"
export TMPDIR=$X11W/tmp IMAKECPP=$X11W/imakecpp XVTC=$W/tc
[ -f "$XC/exports/lib/libX11.a" ] || { echo "run x11/build.sh clibs first" >&2; exit 1; }
mkdir -p "$W" "$TMPDIR"
t0=$(date +%s)

if [ ! -f "$XV/.patched" ]; then
	rm -rf "$XV"
	tgz=$(sh "$AUX/images/fetch.sh" xview)
	sp1=$(sh "$AUX/images/fetch.sh" xview-sp1)
	tar xzf "$tgz" -C "$W"
	patch -s -p1 -d "$XV" < "$sp1/xview/patches/xview-3.2p1.4-asv.patch"
	patch -s -p1 -d "$XV" < "$D/xview-amix.diff"
	mkdir -p "$XV/asv/include"
	cp -R "$sp1/xview/asv/include/sys" "$XV/asv/include/"
	cp "$sp1/xview/openwin-menu" "$W/"
	touch "$XV/.patched"
fi
[ -x "$XVTC/bin/m68k-cbm-sysv4-gcc" ] || sh "$D/mktc.sh" "$XVTC"
"$XVTC/bin/m68k-cbm-sysv4-gcc" -O -c "$D/compat.c" -o "$W/compat.o"

# imake in "installed" mode; every Makefile gets the settings below
cd "$XV"
mkdir -p amixbin
cat > amixbin/imake <<EOI
#!/bin/sh
$XC/config/imake/imake -DASVArchitecture -DUseInstalled -I$XV/config -I$XC/config/cf "\$@" || exit 1
[ -f Makefile ] && cat $XV/imake.append >> Makefile
exit 0
EOI
chmod +x amixbin/imake
cat > imake.append <<EOI
  CC             = sh $D/cc.sh
  CCOPTIONS      = -m68020
  XVDESTDIR      = /usr/openwin
  OPENWINHOME    = /usr/openwin
  EXTRA_DEFINES  = -DOPENWINHOME_DEFAULT=\"/usr/openwin\" -DASV -DAMIX -Denviron=_environ
  IMAKE          = $XV/amixbin/imake
  MKDIRHIER      = mkdir -p
  AR             = $AUX/toolchain/bin/m68k-elf-ar cq
  RANLIB         = $AUX/toolchain/bin/m68k-elf-ranlib
  INCLUDES      := -I$XV/build/include -I$XV/asv/include -I$XC/exports/include \$(INCLUDES)
  LOCAL_LDFLAGS := -L$XV/lib/libxview -L$XV/lib/libolgx -L$XC/exports/lib \$(LOCAL_LDFLAGS)
  USRLIBDIR      = $XC/exports/lib
  SYSV_CLIENT_LIB =
  LEXLIB         = $SYS/usr/ccs/lib/libl.a
  EXTRA_LIBRARIES = -lsocket -lnsl $W/compat.o
  EXTRA_LOAD_FLAGS =
EOI
PATH=$XV/amixbin:$PATH; export PATH
AMIXLD_EXTRA=$X11W/libextra.a; export AMIXLD_EXTRA

(cd config && imake) > imake.log 2>&1
imake >> imake.log 2>&1
$MAKE Makefiles > Makefiles.log 2>&1
$MAKE includes > includes.log 2>&1
for d in lib/libolgx lib/libxview; do
	# the subdirectory loop does not pass a failure up
	(cd $d && $MAKE) > $d/build.log 2>&1 && ! grep -q '^make.*\*\*\*' $d/build.log ||
		{ echo "build failed in $d, see $d/build.log"; exit 1; }
done
(cd clients && imake -DTOPDIR=.. -DCURDIR=./clients && $MAKE Makefiles) > clients/Makefiles.log 2>&1
for c in $CLIENTS; do
	(cd clients/$c && $MAKE) > clients/$c/build.log 2>&1 || { echo "build failed: $c, see clients/$c/build.log"; exit 1; }
done
(cd clients/olvwm-4.1 && imake -DTOPDIR=../.. -DCURDIR=./clients/olvwm-4.1 && $MAKE) \
	> clients/olvwm-4.1/build.log 2>&1 || { echo "build failed: olvwm"; exit 1; }

P=$W/dist/usr/openwin
rm -rf "$W/dist"; mkdir -p "$P/bin" "$P/lib/help" "$P/lib/locale/C/xview"
for c in $CLIENTS; do cp clients/$c/$c "$P/bin/"; done
cp clients/olvwm-4.1/olvwm "$P/bin/"
"$AUX/toolchain/bin/m68k-elf-strip" "$P"/bin/*
cp "$W/openwin-menu" "$P/lib/"
cp misc/support/*.info clients/olvwm-4.1/olvwm.info "$P/lib/help/"
# the OPEN LOOK glyph and cursor fonts (olwm and olvwm stop without
# them) and Lucida, XView's default; the session adds them to the font path
for d in misc 75dpi; do
	mkdir -p "$P/lib/fonts/$d"
	cp fonts/bdf/$d/*.bdf fonts/bdf/$d/fonts.alias "$P/lib/fonts/$d/"
	(cd "$P/lib/fonts/$d" && for f in *.bdf; do
		printf '%s %s\n' "$f" "$(sed -n 's/^FONT //p' "$f" | head -1 | tr A-Z a-z)"
	done > fonts.tmp && { wc -l < fonts.tmp | tr -d ' '; cat fonts.tmp; } > fonts.dir &&
	rm fonts.tmp)
done
cp "$(sh "$AUX/images/fetch.sh" xview-sp1)/xview/text_extras_menu" "$P/lib/locale/C/xview/.text_extras_menu"
python3 "$AUX/x11/mkpkg.py" XVIEW 3.2p1.4 x11 "XView 3.2 and OPEN LOOK" \
	"XView toolkit clients: olwm, olvwm, cmdtool, textedit, clock, props; in /usr/openwin/bin" \
	"$W/dist" "$W/XVIEW.pkg"
du -sk "$P"; ls -l "$W/XVIEW.pkg"
echo "=== xview done in $(( $(date +%s) - t0 )) s"
