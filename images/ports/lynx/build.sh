#!/bin/sh
# build.sh -- cross-build Lynx 2.8.5rel.1 for /opt/amix: curses, no TLS.
#
#   sh images/ports/lynx/build.sh outdir
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
PORT=lynx OUT=$1
. "$AUX/images/ports/port.sh"
R=$ROOT

# the character-set tables come from a host build of makeuctb
src lynx > /dev/null
mv "$W/lynx2-8-5" "$W/lynxhost"
log host sh -c "cd '$W/lynxhost' && CFLAGS='-O -w' ./configure --disable-nls --without-ssl &&
	make -C src/chrtrans CFLAGS='-O -w' makeuctb"
src lynx > /dev/null
S=$W/lynx2-8-5
(cd "$S" && patch -p1 -s < "$D/lynx.diff")
log conf sh -c "cd '$S' && CC='$CC' CFLAGS='$PCFLAGS' ./configure $HOST \
	--with-screen=curses --disable-nls --without-ssl --disable-ipv6 --without-zlib \
	--without-bzlib --disable-full-paths"
cp "$W/lynxhost/src/chrtrans/makeuctb" "$S/src/chrtrans/"
touch "$S/src/chrtrans/makeuctb.o" "$S/config.cache"
touch "$S/src/chrtrans/makeuctb"
log make sh -c "cd '$S' && $NICE make"
mkdir -p "$R/opt/amix/bin" "$R/opt/amix/lib"
cp "$S/lynx" "$R/opt/amix/bin/"
cp "$S/lynx.cfg" "$R/opt/amix/lib/"
# its media-type quality factors
FPU=lynx
portpkg
