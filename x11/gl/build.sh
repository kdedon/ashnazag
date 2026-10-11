#!/bin/sh
# build.sh -- package Mesa's glxgears (gl/build.sh, Xlib libGL) for X.
#
#   sh x11/gl/build.sh
#
# Out: $X11W/gl/GL.pkg (/usr/x11r6/bin/glxgears).  GLW as gl/build.sh.
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
PLATFORM=${PLATFORM:-mac}
X11W=${X11W:-$AUX/images/work/x11-$PLATFORM}
GLW=${GLW:-$AUX/images/work/gl}
W=$X11W/gl
X11W=$X11W GLW=$GLW sh "$AUX/gl/build.sh" > "$X11W/gl.log" 2>&1 ||
	{ tail "$X11W/gl.log"; exit 1; }
[ -f "$GLW/bin/glxgears" ] || { echo "no glxgears: needs the Mesa demos (tools/setup.sh --gl)" >&2; exit 1; }
rm -rf "$W"; mkdir -p "$W/root/usr/x11r6/bin"
"$AUX/toolchain/bin/m68k-elf-strip" -o "$W/root/usr/x11r6/bin/glxgears" "$GLW/bin/glxgears"
python3 "$AUX/x11/mkpkg.py" GL 5.0.2 x11 "Mesa 5.0.2 glxgears" \
	"OpenGL gears demo on Mesa's software renderer; /usr/x11r6/bin/glxgears" "$W/root" "$W/GL.pkg"
ls -l "$W/GL.pkg"
