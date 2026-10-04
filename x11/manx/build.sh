#!/bin/sh
# build.sh -- cross-build manx (text and X11 web browser) as static AMIX
# programs and package it.
#
#   sh x11/manx/build.sh
#
# manx is C99: the m68k Linux gcc compiles to assembly for SVR4 structure
# layout (-malign-int), the AMIX ld links against the static libc and the
# X11R6.3 libX11 of x11/build.sh's clibs stage.  Source:
# images/vendor.manifest (manx).  Out: $X11W/manx/MANX.pkg (/opt/amix).
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../.." && pwd)
X11W=${X11W:-$AUX/images/work/x11}
XC=$X11W/src/xc
W=$X11W/manx
TC=$AUX/toolchain/amix
L=$AUX/toolchain/linux/bin
[ -f "$XC/exports/lib/libX11.a" ] || { echo "run x11/build.sh clibs first" >&2; exit 1; }
t0=$(date +%s)

# the sysroot manx expects: AMIX's, with X under usr/X
R=$W/sysroot
mkdir -p "$R/usr/X"
for d in include lib ccs; do ln -sfn "$TC/m68k-cbm-sysv4/sysroot/usr/$d" "$R/usr/$d"; done
ln -sfn "$XC/exports/include" "$R/usr/X/include"
ln -sfn "$XC/exports/lib" "$R/usr/X/lib"

src=$(sh "$AUX/images/fetch.sh" manx)
rm -rf "$W/src"
cp -R "$src" "$W/src"
mkdir -p "$X11W/tmp"
(cd "$W/src" && TMPDIR=$X11W/tmp nice -n 19 make -j1 TARGET=sysv4 AMIX_SYSROOT="$R" ASV_CROSS="$TC" \
	MINT_GCC="$L/m68k-linux-gnu-gcc" SVR4_AS="$L/m68k-linux-gnu-as" AR="$AUX/toolchain/bin/m68k-elf-ar" \
	build/sysv4/manx build/sysv4/xmanx build/sysv4/manxtrust) > "$W/build.log" 2>&1 ||
	{ echo "build failed, see $W/build.log"; exit 1; }

P=$W/root/opt/amix
rm -rf "$W/root"; mkdir -p "$P/bin" "$P/doc/manx"
for p in manx xmanx manxtrust; do cp "$W/src/build/sysv4/$p" "$P/bin/"; done
"$AUX/toolchain/bin/m68k-elf-strip" "$P"/bin/*
cp "$W/src/README.md" "$W/src/LICENSE" "$P/doc/manx/"
v=$(git -C "$src" rev-parse --short HEAD)
python3 "$AUX/x11/mkpkg.py" MANX "0.$v" web "Manx web browser" \
	"Text (manx) and X11 (xmanx) web browser with HTTPS; in /opt/amix/bin" "$W/root" "$W/MANX.pkg"
ls -l "$P/bin" "$W/MANX.pkg"
echo "=== manx done in $(( $(date +%s) - t0 )) s"
