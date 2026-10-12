#!/bin/sh
# x43.sh -- the XFree86 4.3.0 GLX/DRI client, server-extension and libdrm
# sources, patched for AMIX.
#
#   sh gl/dri/x43.sh DEST
#
# Out: DEST/xc/... (only the files listed below), from ref/xfree86-4.3/dist
# (tools/setup.sh --gl) and gl/dri/patches/*.diff.  Does nothing when DEST
# is complete.
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
DIST=$AUX/ref/xfree86-4.3/dist
DEST=$1
[ -n "$DEST" ] || { echo "usage: x43.sh DEST" >&2; exit 2; }
[ -f "$DEST/.done" ] && exit 0
for n in 1 3; do
	[ -f "$DIST/X430src-$n.tgz" ] || { echo "$DIST/X430src-$n.tgz missing: sh tools/setup.sh --gl" >&2; exit 1; }
done
rm -rf "$DEST"; mkdir -p "$DEST"
OS=xc/programs/Xserver/hw/xfree86/os-support
nice -n 19 tar xzf "$DIST/X430src-1.tgz" -C "$DEST" \
	xc/include/GL/glx.h xc/include/GL/glxint.h xc/include/GL/glxmd.h \
	xc/include/GL/glxproto.h xc/include/GL/glxtokens.h \
	xc/lib/GL/include/GL/internal/glcore.h \
	xc/lib/GL/glx/glxclient.h xc/lib/GL/glx/glxcmds.c xc/lib/GL/glx/glxext.c \
	xc/lib/GL/glx/xfont.c xc/lib/GL/glx/indirect.h xc/lib/GL/glx/indirect_init.h \
	xc/lib/GL/glx/indirect_wrap.h \
	xc/lib/GL/glx/packsingle.h xc/lib/GL/glx/packrender.h xc/lib/GL/glx/size.h \
	xc/lib/GL/dri/dri_glx.c xc/lib/GL/dri/dri_glx.h xc/lib/GL/dri/dri_util.c \
	xc/lib/GL/dri/dri_util.h xc/lib/GL/dri/XF86dri.c xc/lib/GL/dri/xf86dri.h \
	xc/lib/GL/dri/xf86dristr.h \
	xc/lib/GL/mesa/src/drv/tdfx xc/lib/GL/mesa/src/drv/common/depthtmp.h \
	xc/lib/GL/mesa/src/drv/common/spantmp.h xc/lib/GL/mesa/src/drv/common/stenciltmp.h
nice -n 19 tar xzf "$DIST/X430src-3.tgz" -C "$DEST" \
	xc/programs/Xserver/GL/dri/xf86dri.c xc/programs/Xserver/GL/dri/sarea.h \
	xc/programs/Xserver/GL/include/GL/xf86glx.h \
	$OS/xf86drm.h $OS/linux/drm/xf86drm.c $OS/linux/drm/xf86drmHash.c \
	$OS/linux/drm/xf86drmRandom.c $OS/linux/drm/xf86drmSL.c
for p in "$D"/patches/*.diff; do
	patch -s -p1 -d "$DEST" < "$p"
done
touch "$DEST/.done"
