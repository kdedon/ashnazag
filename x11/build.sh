#!/bin/sh
# build.sh -- cross-build the X11R6.3 AMIX server (Xamix) with the fb backend.
#
#   sh x11/build.sh [stage ...]
#
# Stages (default: all in order): tree, imake, makefiles, libs, server;
# clibs (client libraries) and clients (xdm, xchoose, xdmenv; after
# clibs) on request.
# Work tree: $X11W (default: images/work/x11), sources in
# $X11W/src/xc.  Out: $X11W/src/xc/programs/Xserver/Xamix.
set -e

D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/.." && pwd)
X11W=${X11W:-$AUX/images/work/x11}
XC=$X11W/src/xc
TC=$AUX/toolchain/amix
SYS=$TC/m68k-cbm-sysv4/sysroot
MAKE="nice -n 19 make -j1"
TMPDIR=$X11W/tmp
IMAKECPP=$X11W/imakecpp
export TMPDIR IMAKECPP
mkdir -p "$TMPDIR"

tree() {
	[ -d "$XC" ] && { echo "== tree: $XC exists"; return 0; }
	mkdir -p "$X11W/src"
	for a in 1 2 3; do
		nice -n 19 tar xzf "$AUX/ref/x11r6.3/dist/xc-$a.tar.gz" -C "$X11W/src"
	done
	(cd "$AUX/ref/x11r6.3-amix/overlay" && tar cf - xc) | (cd "$X11W/src" && tar xpf -)
	chmod -R u+w "$XC"
	for p in "$D"/patches/*.diff; do
		echo "== patch $(basename "$p")"
		patch -s -p1 -d "$XC" < "$p"
	done
	# interleaved 8-bitplane frame buffers: the 4-plane sources built for 8
	I=$AUX/ref/xfree86-3.3.6/xc/programs/Xserver/iplan2p4
	mkdir -p "$XC/programs/Xserver/iplan2p8"
	cp "$I"/*.[ch] "$XC/programs/Xserver/iplan2p8/"
	{ echo '#define IPlanes 8'; cat "$I/Imakefile"; } > "$XC/programs/Xserver/iplan2p8/Imakefile"
	# new files
	(cd "$D/src" && tar cf - .) | (cd "$XC/programs/Xserver/hw/amix" && tar xpf -)
	cp "$AUX/kernel/mac/display/dsio.h" "$XC/programs/Xserver/hw/amix/rtg/fb/dsio.h"
}

# the config file for this cross build, from the repository's copy
crossdef() {
	sed -e "s,@AMIXBIN@,$TC/bin,g" \
	    -e "s,@SYSINC@,$X11W/sysinc,g" \
	    -e "s,@ELFBIN@,$AUX/toolchain/bin,g" \
	    -e "s,@SYSLIBS@,-lsocket -lnsl -L$X11W/sysinc/lib -lscreen,g" \
		"$D/config/cross.def" > "$XC/config/cf/cross.def"
}

# xdm's patch, if the tree was made without it
newpatches() {
	for p in "$D"/patches/*-xdm.diff; do
		patch -s -p1 -R --dry-run -d "$XC" < "$p" > /dev/null 2>&1 && continue
		echo "== patch $(basename "$p")"
		patch -s -p1 -d "$XC" < "$p"
	done
}

imake() {
	cd "$XC/config/imake"
	nice -n 19 gcc -w -O -DAMIX -DSVR4 -Dm68k -Ulinux -U__linux__ \
		-I../../include -o imake imake.c
	# host cpp without host predefines stands in for AMIX's gcc-cpp
	printf '#!/bin/sh\nexec /usr/bin/cpp -undef "$@"\n' > "$X11W/imakecpp"
	chmod +x "$X11W/imakecpp"
	# the server's archive-only libc members (libc.so holds them beside libc.so.1)
	if [ ! -f "$X11W/libextra.a" ]; then
		rm -rf "$TMPDIR/extra"; mkdir -p "$TMPDIR/extra"
		(cd "$TMPDIR/extra" && $AUX/toolchain/bin/m68k-elf-ar x "$SYS/usr/ccs/lib/libc.so" && rm -f libc.so.1 &&
		 $AUX/toolchain/bin/m68k-elf-ar rc "$X11W/libextra.a" $(ar t "$SYS/usr/ccs/lib/libc.so" | grep -v '^libc.so.1$'))
		rm -rf "$TMPDIR/extra"
	fi
	# libscreen's header, which the sysroot links to an absolute path
	mkdir -p "$X11W/sysinc/amiga"
	(cd "$X11W/sysinc" && cpio -i --quiet --to-stdout usr/amiga/include/amiga/screen.h \
		< "${AMIX_TAPE:-$AUX/kernel/mac/diskroot/build/tape}/02" > amiga/screen.h)
	mkdir -p "$X11W/sysinc/lib"
	(cd "$X11W/sysinc" && cpio -i --quiet --to-stdout usr/amiga/lib/libscreen.so \
		< "${AMIX_TAPE:-$AUX/kernel/mac/diskroot/build/tape}/02" > lib/libscreen.so)
	crossdef
	grep -q 'include <cross.def>' "$XC/config/cf/site.def" ||
		sed -i 's,^#ifdef AfterVendorCF$,&\n#include <cross.def>,' "$XC/config/cf/site.def"
}

IMAKECMD() {
	IMAKECPP=$X11W/imakecpp "$XC/config/imake/imake" -I"$XC/config/cf" \
		-DTOPDIR=$XC -DCURDIR=. "$@"
}

makefiles() {
	cd "$XC"
	IMAKECPP=$X11W/imakecpp PATH="$XC/config/imake:$PATH" \
		"$XC/config/imake/imake" -I./config/cf -DTOPDIR=. -DCURDIR=.
	for d in include lib/xtrans lib/Xau lib/Xdmcp lib/font programs/Xserver; do
		echo "== Makefiles $d"
		(cd "$d" && sub_makefile)
	done
}

# Makefile in the current directory, then its subdirectories
sub_makefile() {
	rel=$(pwd | sed "s,^$XC/*,,")
	up=$(echo "$rel" | sed 's,[^/][^/]*,..,g')
	IMAKECPP=$X11W/imakecpp "$XC/config/imake/imake" -I"$up/config/cf" \
		-DTOPDIR="$up" -DCURDIR="$rel"
	$MAKE -s Makefiles >/dev/null
}

libs() {
	cd "$XC"
	for d in include lib/xtrans lib/Xau lib/Xdmcp lib/font; do
		echo "== includes $d"
		(cd "$d" && $MAKE includes)
	done
	for d in lib/Xau lib/Xdmcp lib/font; do
		echo "== build $d"
		(cd "$d" && $MAKE all)
	done
}

# client libraries for packages: X11 and Xext, Xt's headers; the two
# generators run on the host
clibs() {
	cd "$XC"
	for d in lib/X11 lib/Xext lib/Xt; do
		(cd "$d" && sub_makefile)
	done
	gcc -O -w -Iinclude -o config/util/makestrs config/util/makestrs.c
	gcc -O -w -Iexports/include -o "$X11W/makekeys" lib/X11/util/makekeys.c
	"$X11W/makekeys" < exports/include/X11/keysymdef.h > lib/X11/ks_tables.h
	for d in lib/X11 lib/Xext lib/Xt; do
		(cd "$d" && $MAKE -o ks_tables.h includes)
	done
	for d in lib/X11 lib/Xext; do
		(cd "$d" && $MAKE -o ks_tables.h all)
	done
}

# Xt's dependants and Athena, xdm with its greeter linked in, the session tools
clients() {
	crossdef
	newpatches
	cd "$XC"
	for d in lib/ICE lib/SM lib/Xmu lib/Xaw programs/xdm; do
		(cd "$d" && sub_makefile)
	done
	for d in lib/ICE lib/SM lib/Xmu lib/Xaw; do
		(cd "$d" && $MAKE includes)
	done
	for d in lib/ICE lib/SM lib/Xt lib/Xmu lib/Xaw; do
		(cd "$d" && $MAKE all)
	done
	link="env AMIXLD_EXTRA=$X11W/libextra.a sh $D/config/amixld.sh"
	(cd programs/xdm && $MAKE clean > /dev/null && $MAKE xdm XMULIB=-lXmu EXTRA_INCLUDES=-Igreeter SYS_LIBRARIES="-lsocket -lnsl" CCLINK="$link")
	cc="$TC/bin/m68k-cbm-sysv4-gcc -O -m68020 -I$XC/exports/include -I$X11W/sysinc"
	L=$XC/exports/lib
	mkdir -p "$X11W/session"
	$cc -c -o "$X11W/session/xchoose.o" "$D/session/xchoose.c"
	$link -o "$X11W/session/xchoose" "$X11W/session/xchoose.o" $L/libXaw.a $L/libXmu.a \
		$L/libXt.a $L/libSM.a $L/libICE.a $L/libXext.a $L/libX11.a -lsocket -lnsl
	$cc -I"$AUX/kernel/mac/display" -c -o "$X11W/session/xdmenv.o" "$D/session/xdmenv.c"
	$link -o "$X11W/session/xdmenv" "$X11W/session/xdmenv.o"
	ls -l programs/xdm/xdm "$X11W/session/xchoose" "$X11W/session/xdmenv"
}

server() {
	cd "$XC/programs/Xserver"
	$MAKE includes
	$MAKE all CCLINK="env AMIXLD_EXTRA=$X11W/libextra.a sh $D/config/amixld.sh"
	ls -l Xamix
}

[ $# -gt 0 ] || set -- tree imake makefiles libs server
t0=$(date +%s)
for s; do
	echo "=== $s"
	$s
done
echo "=== done in $(( $(date +%s) - t0 )) s"
