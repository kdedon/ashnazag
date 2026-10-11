#!/bin/sh
# build.sh -- cross-build Mesa 5.0.2 for AMIX with gcc 2.95.3.
#
#   sh gl/build.sh
#
# Needs toolchain/amix-gcc295 (toolchain/build-gcc295.sh) and the Mesa
# source from tools/setup.sh --gl.  Work tree: $GLW (default
# images/work/gl).  Out in $GLW: lib/libOSMesa.a (core, software
# rasteriser, OSMesa, snprintf; no threads), lib/libGL.a (the same with the
# Xlib driver, when $X11W has the X client libraries), bin/glxgears (with
# the Mesa demos) and include/.
set -e

D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/.." && pwd)
GLW=${GLW:-$AUX/images/work/gl}
X11W=${X11W:-$AUX/images/work/x11-mac}
TGZ=$AUX/ref/mesa/dist/MesaLib-5.0.2.tar.gz
TC=$AUX/toolchain/amix-gcc295
T=m68k-cbm-sysv4
CC=$TC/bin/$T-gcc
AR=$TC/bin/$T-ar
N="nice -n 19"
# AMIX's math.h has M_PI only outside strict ANSI or under X/Open
CFLAGS="-O2 -m68040 -fomit-frame-pointer -D_XOPEN_SOURCE -DBYTE_ORDER=4321 -DBIG_ENDIAN=4321"
TMPDIR=$GLW/tmp
export TMPDIR

[ -x "$CC" ] || { echo "$CC missing: sh toolchain/build-gcc295.sh" >&2; exit 1; }
[ -f "$TGZ" ] || { echo "$TGZ missing: sh tools/setup.sh --gl" >&2; exit 1; }
M=$GLW/Mesa-5.0.2
mkdir -p "$TMPDIR" "$GLW/obj" "$GLW/lib"
if [ ! -d "$M" ]; then
	$N tar xzf "$TGZ" -C "$GLW"
	for p in "$D"/patches/*.diff; do
		[ -f "$p" ] || continue
		echo "== patch $(basename "$p")"
		patch -s -p1 -d "$M" < "$p"
	done
	# the compiler driver searches the system headers first, and AMIX has
	# a macros.h of its own
	for f in "$M"/src/*/*.[ch]; do
		grep -q '^#include "macros.h"' "$f" &&
			sed -i 's,^#include "macros.h",#include "../macros.h",' "$f"
	done
fi

S=$M/src
CORE="api_arrayelt api_loopback api_noop api_validate accum attrib blend
	buffers clip colortab context convolve debug depth dispatch dlist drawpix
	enable enums eval extensions feedback fog get glapi glthread hash hint
	histogram image imports light lines matrix mmath pixel points polygon
	rastpos state stencil texcompress texformat teximage texobj texstate
	texstore texutil varray vpexec vpparse vpstate vtxfmt
	math/m_debug_clip math/m_debug_norm math/m_debug_xform math/m_eval
	math/m_matrix math/m_translate math/m_vector math/m_xform
	array_cache/ac_context array_cache/ac_import
	swrast/s_aaline swrast/s_aatriangle swrast/s_accum swrast/s_alpha
	swrast/s_alphabuf swrast/s_bitmap swrast/s_blend swrast/s_buffers
	swrast/s_copypix swrast/s_context swrast/s_depth swrast/s_drawpix
	swrast/s_feedback swrast/s_fog swrast/s_histogram swrast/s_imaging
	swrast/s_lines swrast/s_logic swrast/s_masking swrast/s_pixeltex
	swrast/s_points swrast/s_readpix swrast/s_span swrast/s_stencil
	swrast/s_texture swrast/s_texstore swrast/s_triangle swrast/s_zoom
	swrast_setup/ss_context swrast_setup/ss_triangle swrast_setup/ss_vb
	tnl/t_array_api tnl/t_array_import tnl/t_context tnl/t_eval_api
	tnl/t_imm_alloc tnl/t_imm_api tnl/t_imm_debug tnl/t_imm_dlist
	tnl/t_imm_elt tnl/t_imm_eval tnl/t_imm_exec tnl/t_imm_fixup
	tnl/t_pipeline tnl/t_vb_fog tnl/t_vb_light tnl/t_vb_normals
	tnl/t_vb_points tnl/t_vb_program tnl/t_vb_render tnl/t_vb_texgen
	tnl/t_vb_texmat tnl/t_vb_vertex"
XLIB="X/glxapi X/fakeglx X/xfonts X/xm_api X/xm_dd X/xm_line X/xm_span X/xm_tri"

objs() {	# extra cflags, sources -> $OBJS
	fl=$1; shift
	OBJS=
	for f in $*; do
		o=$GLW/obj/$(echo $f | tr / _).o
		if [ ! -f "$o" ] || [ "$S/$f.c" -nt "$o" ]; then
			echo "cc $f.c"
			$N "$CC" $CFLAGS $fl -I"$M/include" -I"$S" -c "$S/$f.c" -o "$o"
		fi
		OBJS="$OBJS $o"
	done
}

lib() {		# name, objects
	l=$GLW/lib/$1; shift
	rm -f "$l"
	$N "$AR" rc "$l" "$@"
	echo "== $l"
}

objs "" $CORE
COBJS="$OBJS $GLW/obj/snprintf.o"
$N "$CC" $CFLAGS -c "$D/src/snprintf.c" -o "$GLW/obj/snprintf.o"
objs "" OSmesa/osmesa
lib libOSMesa.a $COBJS $OBJS
rm -rf "$GLW/include"
cp -r "$M/include" "$GLW/include"

# libGL: the Xlib driver against the X11R6.3 client libraries (x11/build.sh
# clibs), drawing with core X requests
XINC=$X11W/src/xc/exports/include
XL=$X11W/src/xc/lib
[ -f "$XL/X11/libX11.a" ] || exit 0
objs "-I$XINC" $XLIB
lib libGL.a $COBJS $OBJS

# glxgears, linked like the system's programs: shared libc, then the
# members only its archive has
DEMOS=$AUX/ref/mesa/dist/MesaDemos-5.0.2.tar.gz
[ -f "$DEMOS" ] || exit 0
OLD=$AUX/toolchain/amix
SYS=$OLD/m68k-cbm-sysv4/sysroot
LIBGCC=$(ls "$TC"/lib/gcc-lib/$T/*/libgcc.a)
mkdir -p "$GLW/bin" "$GLW/extra"
$N tar xzf "$DEMOS" -C "$GLW" Mesa-5.0.2/xdemos/glxgears.c
patch -s -p1 -d "$M" < "$D/demos/glxgears.diff"
if [ ! -f "$GLW/libextra.a" ]; then
	(cd "$GLW/extra" && ar x "$SYS/usr/ccs/lib/libc.so" && rm -f libc.so.1 &&
	 ar rc ../libextra.a *)
fi
$N "$CC" $CFLAGS -I"$GLW/include" -I"$XINC" -c "$M/xdemos/glxgears.c" -o "$GLW/obj/glxgears.o"
$N "$OLD/bin/$T-ld" -o "$GLW/bin/glxgears" "$SYS/usr/ccs/lib/crt1.o" "$SYS/usr/ccs/lib/crti.o" \
	"$GLW/obj/glxgears.o" "$GLW/lib/libGL.a" "$XL/Xext/libXext.a" "$XL/X11/libX11.a" \
	-L"$SYS/usr/lib" -lsocket -lnsl "$SYS/usr/lib/libc.so.1" "$SYS/usr/ccs/lib/libm.a" \
	"$GLW/libextra.a" "$LIBGCC" "$SYS/usr/ccs/lib/crtn.o"
echo "== $GLW/bin/glxgears"
