#!/bin/sh
# glide.sh -- the Glide 3 sources for the Voodoo3/Banshee (h3), DRI build,
# in one directory, patched for AMIX.
#
#   sh gl/glide/glide.sh DEST
#
# Out: DEST/*.[ch], from ref/glide/dist (tools/setup.sh --gl) and
# gl/glide/patches/*.diff.  When DEST is complete, gl/glide/src is copied
# again and patches it lacks are applied.
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
TGZ=$AUX/ref/glide/dist/glide_2002.04.10.orig.tar.gz
DEST=$1
[ -n "$DEST" ] || { echo "usage: glide.sh DEST" >&2; exit 2; }
# files of our own are refreshed in a complete tree
if [ -f "$DEST/.done" ]; then
	for f in "$D"/src/*; do
		cmp -s "$f" "$DEST/${f##*/}" || cp "$f" "$DEST/"
	done
	# patches added since the tree was made
	for p in "$D"/patches/*.diff; do
		patch -s -p1 -R --dry-run -d "$DEST" < "$p" > /dev/null 2>&1 && continue
		patch -s -p1 -d "$DEST" < "$p"
	done
	exit 0
fi
[ -f "$TGZ" ] || { echo "$TGZ missing: sh tools/setup.sh --gl" >&2; exit 1; }
rm -rf "$DEST" "$DEST.x"; mkdir -p "$DEST" "$DEST.x"
U=glide-2002.04.10/upstream/tarballs
nice -n 19 tar xzf "$TGZ" -C "$DEST.x" $U/glide3x.2002.04.10.tar.gz $U/swlibs.2001.01.26.tar.gz
nice -n 19 tar xzf "$DEST.x/$U/glide3x.2002.04.10.tar.gz" -C "$DEST.x" glide3x/h3
nice -n 19 tar xzf "$DEST.x/$U/swlibs.2001.01.26.tar.gz" -C "$DEST.x" swlibs/fxmisc swlibs/newpci
H=$DEST.x/glide3x/h3
G=$H/glide3/src
for f in diget diglide disst distate distrip ditex fifo g3df gaa gbanner gdraw gerror \
	gpci gsfc gsplash gstrip gtex gtexdl gthread gu gxdraw xtexdl_def; do
	cp "$G/$f.c" "$DEST/"
done
for f in gglide glfb gsst; do
	cp "$G/$f.c.dri" "$DEST/$f.c"
done
cp "$H/minihwc/linhwc.c.dri" "$DEST/linhwc.c"
cp "$H/minihwc/gdebug.c" "$H"/minihwc/*.h "$H"/incsrc/*.h \
	"$DEST.x"/swlibs/fxmisc/*.h "$DEST.x"/swlibs/newpci/pcilib/*.h "$DEST/"
cp "$G"/*.h "$DEST/"
rm -rf "$DEST.x"
cp "$D"/src/* "$DEST/"
for p in "$D"/patches/*.diff; do
	[ -f "$p" ] || continue
	patch -s -p1 -d "$DEST" < "$p"
done
touch "$DEST/.done"
