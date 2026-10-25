#!/bin/sh
# build.sh -- cross-build GNU wget 1.11.4 for /opt/amix: no TLS, rates printed
# without floating point.
#
#   sh images/ports/wget/build.sh outdir
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
PORT=wget OUT=$1
. "$AUX/images/ports/port.sh"
R=$ROOT

src wget > /dev/null
S=$W/wget-1.11.4
(cd "$S" && patch -p1 -s < "$D/wget.diff")
# AMIX has no stdint.h: wget then declares intptr_t itself
log conf sh -c "cd '$S' && CC='$CC' CFLAGS='$PCFLAGS' ac_cv_header_stdint_h=no ./configure $HOST \
	--without-ssl --disable-nls --disable-ipv6 --disable-debug"
log make sh -c "cd '$S/src' && $NICE make"
mkdir -p "$R/opt/amix/bin"
cp "$S/src/wget" "$R/opt/amix/bin/"
# its timers and progress
FPU=wget
portpkg
