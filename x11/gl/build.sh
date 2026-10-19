#!/bin/sh
# build.sh -- package Mesa (gl/build.sh) for X: libGL and glxgears, and a
# DRI driver package per card.
#
#   sh x11/gl/build.sh
#
# Out: $X11W/gl/GL.pkg (/usr/x11r6/lib/libGL.so, /usr/x11r6/bin/glxgears),
# GLTDFX.pkg (Voodoo3/Banshee) and GLR200.pkg (Radeon), each with its
# /usr/x11r6/lib/modules/dri/<card>_dri.so.  GLW as gl/build.sh.
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
PLATFORM=${PLATFORM:-mac}
X11W=${X11W:-$AUX/images/work/x11-$PLATFORM}
GLW=${GLW:-$AUX/images/work/gl}
W=$X11W/gl
STRIP=$AUX/toolchain/bin/m68k-elf-strip
X11W=$X11W GLW=$GLW sh "$AUX/gl/build.sh" > "$X11W/gl.log" 2>&1 ||
	{ tail "$X11W/gl.log"; exit 1; }
[ -f "$GLW/bin/glxgears" ] || { echo "no glxgears: needs the Mesa demos (tools/setup.sh --gl)" >&2; exit 1; }
rm -rf "$W"; mkdir -p "$W/root/usr/x11r6/bin" "$W/root/usr/x11r6/lib"
"$STRIP" -o "$W/root/usr/x11r6/bin/glxgears" "$GLW/bin/glxgears"
"$STRIP" -o "$W/root/usr/x11r6/lib/libGL.so" "$GLW/lib/libGL.so"
python3 "$AUX/x11/mkpkg.py" GL 5.0.2 x11 "Mesa 5.0.2 libGL" \
	"OpenGL for X: libGL.so (Mesa's software renderer, or the card's DRI driver) and glxgears" \
	"$W/root" "$W/GL.pkg"
drv() {	# package, driver, card
	r=$W/$1; mkdir -p "$r/usr/x11r6/lib/modules/dri"
	"$STRIP" -o "$r/usr/x11r6/lib/modules/dri/$2_dri.so" "$GLW/lib/$2_dri.so"
	python3 "$AUX/x11/mkpkg.py" $1 5.0.2 x11 "$3 DRI driver" \
		"Direct rendering for libGL on a $3; /usr/x11r6/lib/modules/dri/$2_dri.so" "$r" "$W/$1.pkg"
}
drv GLTDFX tdfx "Voodoo3/Banshee"
drv GLR200 r200 "Radeon 8500/9000"
ls -l "$W"/*.pkg
