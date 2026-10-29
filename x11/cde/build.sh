#!/bin/sh
# build.sh -- CDE 2.2.4a and Motif 2.3.8 (ref/cde-amix) cross-built with
# gcc14-cross-amix, self-contained: X11R6.3 client libraries built here
# with the same compiler, everything linked statically except AMIX's libc,
# libsocket and libnsl.
#
#   sh x11/cde/build.sh [stage ...]
#
# Stages (default all in order): xlibs, motif, cde, pkg.
# Work tree: $CDEW (default images/work/cde).  Out: $CDEW/CDE.pkg, copied
# to $X11W/cde/CDE.pkg for the image builders (XPKGS=cde).
# GCC14: toolchain/build-gcc14.sh's install (default toolchain/gcc14).
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
PLATFORM=${PLATFORM:-mac}
X11W=${X11W:-$AUX/images/work/x11-$PLATFORM}
CDEW=${CDEW:-$AUX/images/work/cde}
GCC14=${GCC14:-$AUX/toolchain/gcc14}
SYSROOT=$GCC14/sysroot
XC=$CDEW/x11/xc
UP=$CDEW/cde-amix
MAKE="nice -n 19 make -j1"
PATH=$GCC14/bin:$PATH
TMPDIR=$CDEW/tmp
export PATH TMPDIR
[ -x "$GCC14/bin/m68k-elf-gcc" ] || { echo "no $GCC14/bin/m68k-elf-gcc" >&2; exit 1; }
mkdir -p "$TMPDIR"

# X11R6.3 client libraries: X11, Xext, Xau, Xdmcp, ICE, SM, Xt, Xmu;
# Xmu takes a header from Xaw
XLIBS="lib/xtrans lib/Xau lib/Xdmcp lib/X11 lib/Xext lib/ICE lib/SM lib/Xt lib/Xmu"
xlibs() {
	rm -rf "$CDEW/x11"; mkdir -p "$CDEW/x11"
	for a in 1 2 3; do
		nice -n 19 tar xzf "$AUX/ref/x11r6.3/dist/xc-$a.tar.gz" -C "$CDEW/x11"
	done
	(cd "$AUX/ref/x11r6.3-amix/overlay" && tar cf - xc) | (cd "$CDEW/x11" && tar xpf -)
	chmod -R u+w "$XC"
	for p in "$D"/patches/x11/*.diff; do patch -s -p1 -d "$XC" < "$p"; done
	cd "$XC/config/imake"
	nice -n 19 gcc -w -O -DAMIX -DSVR4 -Dm68k -Ulinux -U__linux__ -I../../include -o imake imake.c
	printf '#!/bin/sh\nexec /usr/bin/cpp -undef "$@"\n' > "$CDEW/x11/imakecpp"
	chmod +x "$CDEW/x11/imakecpp"
	sed "s,@GCC14@,$GCC14,g" "$D/xcross.def" > "$XC/config/cf/cross.def"
	sed -i 's,^#ifdef AfterVendorCF$,&\n#include <cross.def>,' "$XC/config/cf/site.def"
	export IMAKECPP=$CDEW/x11/imakecpp
	cd "$XC"
	config/imake/imake -I./config/cf -DTOPDIR=. -DCURDIR=.
	for d in include $XLIBS lib/Xaw; do
		up=$(echo "$d" | sed 's,[^/][^/]*,..,g')
		(cd "$d" && "$XC/config/imake/imake" -I"$up/config/cf" -DTOPDIR="$up" -DCURDIR="$d" &&
		 $MAKE -s Makefiles > /dev/null)
	done
	gcc -O -w -Iinclude -o config/util/makestrs config/util/makestrs.c
	gcc -O -w -Iinclude -o "$CDEW/x11/makekeys" lib/X11/util/makekeys.c
	(cd include && $MAKE includes) > "$CDEW/x11/includes.log" 2>&1
	"$CDEW/x11/makekeys" < exports/include/X11/keysymdef.h > lib/X11/ks_tables.h
	for d in $XLIBS lib/Xaw; do
		(cd "$d" && $MAKE -o ks_tables.h includes) >> "$CDEW/x11/includes.log" 2>&1
	done
	for d in $XLIBS; do
		echo "== $d"
		(cd "$d" && $MAKE -o ks_tables.h all) > "$CDEW/x11/$(basename "$d").log" 2>&1 ||
			{ tail -20 "$CDEW/x11/$(basename "$d").log"; exit 1; }
	done
	ls -l exports/lib/*.a
}

# ref/cde-amix with our patches, configured for this tree
upstream() {
	rm -rf "$UP"
	cp -R "$AUX/ref/cde-amix" "$UP"
	rm -rf "$UP/.git"
	for p in "$D"/patches/*.diff; do
		[ -f "$p" ] || continue
		patch -s -p1 -d "$UP" < "$p"
	done
	for p in "$D"/patches/cde/*.patch "$D"/patches/motif/*.patch; do
		[ -f "$p" ] || continue
		cp "$p" "$UP/patches/$(basename "$(dirname "$p")")/"
	done
	cat > "$UP/config.sh" <<E
TOOLCHAIN=$GCC14
SYSROOT=$SYSROOT
X11_EXPORTS=$XC/exports
X11_SRC=$XC
DL=$CDEW/dl
WORK=$CDEW/work
JOBS=1
E
}

motif() {
	upstream
	sh "$UP/fetch.sh"
	nice -n 19 sh "$UP/scripts/build-motif.sh"
	nice -n 19 sh "$UP/scripts/build-libxp.sh"
}

cde() {
	upstream
	nice -n 19 sh "$UP/scripts/build-cde.sh"
}

pkg() {
	nice -n 19 sh "$UP/scripts/package-cde.sh"
	sh "$D/mkcdepkg.sh" "$CDEW"
	mkdir -p "$X11W/cde"
	cp "$CDEW/CDE.pkg" "$X11W/cde/CDE.pkg"
}

[ $# -gt 0 ] || set -- xlibs motif cde pkg
t0=$(date +%s)
for s; do
	echo "=== $s"
	$s
done
echo "=== done in $(( $(date +%s) - t0 )) s"
