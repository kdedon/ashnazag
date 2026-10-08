#!/bin/sh
# freetype.sh -- FreeType's sources for the Win16 environment's TrueType
# (ttf.c): the pinned release, fetched once into toolchain/dl, unpacked
# into toolchain/src.  Prints the source directory, or with `srcs' the
# files to compile and with `flags' the compiler options they and ttf.c
# take.
#
#   sh kernel/guest/win16/ft/freetype.sh [srcs | flags]
#
# Only part of it is built, configured by w16ftopt.h and w16ftmod.h: the
# TrueType driver with its bytecode interpreter, the sfnt tables and the
# black-and-white rasterizer, over FreeType's base.
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../../.." && pwd)
FT_V=2.13.3
FT_URL=https://downloads.sourceforge.net/project/freetype/freetype2/$FT_V/freetype-$FT_V.tar.xz
FT_SUM=0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289
DL=$AUX/toolchain/dl
SRC=$AUX/toolchain/src/freetype-$FT_V
if [ ! -f "$SRC/src/truetype/truetype.c" ]; then
	mkdir -p "$DL" "$AUX/toolchain/src"
	f=$DL/freetype-$FT_V.tar.xz
	if ! echo "$FT_SUM  $f" | sha256sum -c --status 2>/dev/null; then
		curl -fsSL -m 600 -o "$f.part" "$FT_URL" || { echo "freetype.sh: download failed: $FT_URL" >&2; exit 1; }
		mv "$f.part" "$f"
		echo "$FT_SUM  $f" | sha256sum -c --quiet || { rm -f "$f"; echo "freetype.sh: $f: sha256 mismatch" >&2; exit 1; }
	fi
	tar -xJf "$f" -C "$AUX/toolchain/src"
fi
case $1 in
srcs)
	for f in base/ftsystem base/ftinit base/ftdebug base/ftbase sfnt/sfnt truetype/truetype raster/raster; do
		echo "$SRC/src/$f.c"
	done ;;
flags)
	echo "-I$D -I$SRC/include -DFT2_BUILD_LIBRARY -DFT_CONFIG_OPTIONS_H=<w16ftopt.h> -DFT_CONFIG_MODULES_H=<w16ftmod.h>" ;;
*)
	echo "$SRC" ;;
esac
