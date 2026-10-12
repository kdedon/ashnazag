#!/bin/sh
# build.sh -- cross-build Mesa 5.0.2 for AMIX with gcc 2.95.3.
#
#   sh gl/build.sh
#
# Needs toolchain/amix-gcc295 (toolchain/build-gcc295.sh) and the Mesa
# source from tools/setup.sh --gl.  Work tree: $GLW (default
# images/work/gl).  Out in $GLW: lib/libOSMesa.a (core, software
# rasteriser, OSMesa, snprintf; no threads), lib/libGL.a (the same with the
# Xlib driver, when $X11W has the X client libraries, and direct rendering
# through XFree86 4.3's GLX and DRI client code and libdrm: gl/dri/x43.sh),
# lib/libdrm.a, lib/libdrihost.a (the DRI drivers without X: drihost.h),
# bin/glxgears (with the Mesa demos) and include/.
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
# patches added since the tree was made
for p in "$D"/patches/*.diff; do
	patch -s -p1 -R --dry-run -d "$M" < "$p" > /dev/null 2>&1 && continue
	echo "== patch $(basename "$p")"
	patch -s -p1 -d "$M" < "$p"
done

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
XLIB="X/fakeglx X/xfonts X/xm_api X/xm_dd X/xm_line X/xm_span X/xm_tri"
# the dispatcher picks the real GLX where a DRI driver takes the screen
XGLX="X/glxapi X/realglx"
DRM="xf86drm xf86drmHash xf86drmRandom xf86drmSL"
GLX43="glx/glxcmds glx/glxext glx/xfont dri/dri_glx dri/XF86dri dri/dri_util"

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

# 4.3 sources (dir/name under a source root), objects named x43_*
objs43() {	# extra cflags, source root, sources -> $OBJS
	fl=$1; r=$2; shift 2
	OBJS=
	for f in $*; do
		o=$GLW/obj/x43_$(basename $f).o
		if [ ! -f "$o" ] || [ "$r/$f.c" -nt "$o" ]; then
			echo "cc $f.c"
			$N "$CC" $CFLAGS $fl -I"$M/include" -I"$S" -c "$r/$f.c" -o "$o"
		fi
		OBJS="$OBJS $o"
	done
}

# libGL: the Xlib driver against the X11R6.3 client libraries (x11/build.sh
# clibs), drawing with core X requests, and the GLX and DRI client code for
# a server whose GLX is direct only.  DRI drivers are linked in
# (__driDrivers, gl/dri/src/dri_drivers.c): binutils here makes no shared
# objects, and AMIX's dlopen has no RTLD_GLOBAL to show them libGL.
XINC=$X11W/src/xc/exports/include
XL=$X11W/src/xc/lib
[ -f "$XL/X11/libX11.a" ] || exit 0
X43=$GLW/x43
sh "$D/dri/x43.sh" "$X43"
XC=$X43/xc
OSS=$XC/programs/Xserver/hw/xfree86/os-support
DRIFL="-D__AMIX__ -DGLX_DIRECT_RENDERING -DGLX_BUILT_IN_XMESA -DDRM_USE_MALLOC
	-I$D/dri/include -I$XC/lib/GL/glx -I$XC/lib/GL/dri -I$XC/lib/GL/include
	-I$XC/include -I$XC/programs/Xserver/GL/dri -I$OSS -I$AUX/kernel/drm -I$S/X
	-I$XINC -I$XINC/X11/extensions -I$X11W/src/xc/include/extensions"
objs "-I$XINC" $XLIB
XOBJS=$OBJS
objs "$DRIFL" $XGLX
XOBJS="$XOBJS $OBJS"
# signal.h has no sigset_t under _XOPEN_SOURCE
objs43 "$DRIFL -U_XOPEN_SOURCE" "$OSS/linux/drm" $DRM
DOBJS=$OBJS
lib libdrm.a $DOBJS
objs43 "$DRIFL" "$XC/lib/GL" $GLX43
XOBJS="$XOBJS $OBJS $DOBJS"
# the Voodoo3/Banshee DRI driver and the Glide 3 it draws with
GLIDE=$GLW/glide
sh "$D/glide/glide.sh" "$GLIDE"
GLIDEFL="-D__linux__ -DH3 -DGLIDE_PACKET3_TRI_SETUP=1 -DUSE_PACKET_FIFO=1
	-DGLIDE_HW_TRI_SETUP=1 -DGLIDE_INIT_HWC -DHAL_HW=1 -DGLIDE_TRI_CULLING=1
	-DGLIDE_USE_C_TRISETUP -DGLIDE_LIB -DGLIDE3 -DGLIDE3_ALPHA -DFX_STATIC_BUILD
	-DGLIDE_PACKED_RGB=0 -I$GLIDE"
GOBJS=
GH=$(ls -t "$GLIDE"/*.h | head -1)
for f in diget diglide disst distate distrip ditex fifo g3df gaa gbanner gdraw gerror \
	gglide glfb gpci gsfc gsplash gsst gstrip gtex gtexdl gthread gu gxdraw xtexdl_def \
	linhwc gdebug amixglide amixfront; do
	o=$GLW/obj/glide_$f.o
	if [ ! -f "$o" ] || [ "$GLIDE/$f.c" -nt "$o" ] || [ "$GH" -nt "$o" ]; then
		echo "cc glide/$f.c"
		$N "$CC" $CFLAGS $GLIDEFL -c "$GLIDE/$f.c" -o "$o"
	fi
	GOBJS="$GOBJS $o"
done
TD=$XC/lib/GL/mesa/src/drv/tdfx
# the system's macros.h comes first on the include path
sed -i "s,^#include \"macros.h\",#include \"$S/macros.h\"," "$TD"/*.[ch]
TDFL="$DRIFL -D__driCreateScreen=tdfx_driCreateScreen
	-D__driRegisterExtensions=tdfx_driRegisterExtensions
	-I$XC/lib/GL/mesa/src/drv/common -I$XC/programs/Xserver/GL/include -I$AUX/x11/src/dri"
objs43 "$TDFL" "$TD" tdfx_context tdfx_dd tdfx_lock tdfx_pixels tdfx_render tdfx_screen \
	tdfx_span tdfx_state tdfx_tex tdfx_texman tdfx_texstate tdfx_tris tdfx_vb
TDOBJS="$OBJS $GOBJS"
XOBJS="$XOBJS $TDOBJS"
objs43 "$DRIFL" "$D/dri/src" glxdirect dri_drivers
lib libGL.a $COBJS $XOBJS $OBJS
# the drivers again, for a program that owns a display session and has
# no X server: with libOSMesa.a's core
objs43 "$DRIFL -I$AUX/x11/src/dri" "$D/dri/src" drihost dri_drivers
DUOBJ=$(ls "$GLW"/obj/x43_dri_util.o)
lib libdrihost.a $OBJS $TDOBJS $DUOBJ $DOBJS
rm -rf "$GLW/include/dri"; mkdir -p "$GLW/include/dri"
cp "$XC/lib/GL/glx/glxclient.h" "$XC/lib/GL/dri/dri_glx.h" "$XC/lib/GL/dri/dri_util.h" \
	"$XC/lib/GL/dri/xf86dri.h" "$XC/programs/Xserver/GL/dri/sarea.h" "$OSS/xf86drm.h" \
	"$D/dri/src/drihost.h" "$GLW/include/dri/"

# X GL programs, linked like the system's programs: shared libc, then the
# members only its archive has
OLD=$AUX/toolchain/amix
SYS=$OLD/m68k-cbm-sysv4/sysroot
LIBGCC=$(ls "$TC"/lib/gcc-lib/$T/*/libgcc.a)
mkdir -p "$GLW/bin" "$GLW/extra"
if [ ! -f "$GLW/libextra.a" ]; then
	(cd "$GLW/extra" && ar x "$SYS/usr/ccs/lib/libc.so" && rm -f libc.so.1 &&
	 ar rc ../libextra.a *)
fi
xlink() {	# program, objects
	p=$1; shift
	$N "$OLD/bin/$T-ld" -o "$GLW/bin/$p" "$SYS/usr/ccs/lib/crt1.o" "$SYS/usr/ccs/lib/crti.o" \
		"$@" "$GLW/lib/libGL.a" "$XL/Xext/libXext.a" "$XL/X11/libX11.a" \
		-L"$SYS/usr/lib" -lsocket -lnsl "$SYS/usr/lib/libc.so.1" "$SYS/usr/ccs/lib/libm.a" \
		"$GLW/libextra.a" "$LIBGCC" "$SYS/usr/ccs/lib/crtn.o"
	echo "== $GLW/bin/$p"
}

# DRI set-up without a card driver, and Mesa's renderer without a DRI driver
$N "$CC" $CFLAGS $DRIFL -U_XOPEN_SOURCE -I"$M/include" -I"$S" -I"$AUX/x11/src/dri" \
	-I"$AUX/kernel/mac/display" \
	-c "$D/src/dritest.c" -o "$GLW/obj/dritest.o"
xlink dritest "$GLW/obj/dritest.o"

DEMOS=$AUX/ref/mesa/dist/MesaDemos-5.0.2.tar.gz
[ -f "$DEMOS" ] || exit 0
$N tar xzf "$DEMOS" -C "$GLW" Mesa-5.0.2/xdemos/glxgears.c
patch -s -p1 -d "$M" < "$D/demos/glxgears.diff"
$N "$CC" $CFLAGS -I"$GLW/include" -I"$XINC" -c "$M/xdemos/glxgears.c" -o "$GLW/obj/glxgears.o"
xlink glxgears "$GLW/obj/glxgears.o"
