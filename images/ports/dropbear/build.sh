#!/bin/sh
# build.sh -- cross-build Dropbear 2025.88 (dropbearmulti) for /opt/amix:
# curve25519 and ed25519 only, entropy from prngd over TCP.
#
#   sh images/ports/dropbear/build.sh outdir
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
PORT=dropbear OUT=$1
. "$AUX/images/ports/port.sh"
R=$ROOT

src dropbear > /dev/null
S=$W/dropbear-2025.88
(cd "$S" && patch -p1 -s < "$D/dropbear.diff")
cp "$D/localoptions.h" "$S/"
log conf sh -c "cd '$S' && CC='$CC' CFLAGS='$PCFLAGS -DENDIAN_BIG -DENDIAN_32BITWORD \
	-include $D/amix.h' ./configure $HOST \
	--disable-zlib --disable-harden --disable-lastlog --disable-utmp --disable-utmpx \
	--disable-wtmp --disable-wtmpx --disable-pututline --disable-pututxline \
	--disable-loginfunc --disable-pam --enable-bundled-libtom --disable-largefile \
	--disable-syslog"
log make sh -c "cd '$S' && $NICE make PROGRAMS='dropbear dbclient dropbearkey' MULTI=1 \
	LIBS='-lsocket -lnsl -lgen'"
mkdir -p "$R/opt/amix/sbin" "$R/etc/rc2.d"
cp "$S/dropbearmulti" "$R/opt/amix/sbin/"
cp "$D/S74dropbear" "$R/etc/rc2.d/"
portpkg
