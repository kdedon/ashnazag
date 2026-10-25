#!/bin/sh
# build.sh -- joe 3.7 (with jpico, jmacs, jstar, rjoe) for /opt/amix.
#
#   sh images/ports/joe/build.sh outdir
#
# The patch keeps floating point out of the integer prompts (go to line,
# options, macros): without an FPU they take plain numbers and the math
# prompt works in integers.
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
PORT=joe OUT=$1
. "$AUX/images/ports/port.sh"
S=$W/$(src joe)
(cd "$S" && patch -p1 -s < "$D/joe.diff" &&
	./configure --host=m68k-cbm-sysv4 --prefix=/opt/amix --sysconfdir=/opt/amix/etc \
		--disable-curses --disable-largefile CC="$CC" CPP="$CC -E" CFLAGS="$PCFLAGS -DPROTOTYPES=1" &&
	nice -n 19 make -j1 && make install DESTDIR="$W/dest") > "$W/build.log" 2>&1 ||
	{ tail -20 "$W/build.log"; exit 1; }
! grep 'implicit declaration' "$W/build.log" || exit 1
cp -R "$W/dest/opt" "$ROOT/"
find "$ROOT" -type l -exec rm {} \;
rm -rf "$ROOT/opt/amix/share/locale"
mkdir -p "$ROOT/opt/amix/doc/joe"
cp "$S/COPYING" "$S/README" "$ROOT/opt/amix/doc/joe/"
# the math prompt, used only with an FPU
FPU=joe
portpkg
