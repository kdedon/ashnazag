#!/bin/sh
# build.sh -- GNU screen 4.0.3 for /opt/amix.
#
#   sh images/ports/screen/build.sh outdir
#
# 4.0.3 is the last screen that talks to its sessions over FIFOs; later
# ones need Unix-domain sockets passing descriptors.  config.h comes from
# config.sed: configure's run tests cannot cross-compile.  No load meter.
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
PORT=screen OUT=$1
. "$AUX/images/ports/port.sh"
S=$W/$(src screen)
(cd "$S" && patch -p1 -s < "$D/screen.diff")
sed -f "$D/config.sed" "$S/config.h.in" > "$S/config.h"
sed -e "s|@CC@|$CC|" -e "s|@CPP@|$CC -E|" -e "s|@CFLAGS@|$PCFLAGS|" -e "s|@CPPFLAGS@|$PCFLAGS|" \
	-e 's|@LDFLAGS@||' -e 's|@LIBS@|-lcurses -lcrypt|' -e 's|@AWK@|awk|' -e 's|@prefix@|/opt/amix|' \
	-e 's|@exec_prefix@|/opt/amix|' -e 's|@srcdir@|.|' -e 's|@VERSION@|4.0.3|' \
	-e 's|@INSTALL@|install|' -e 's|@INSTALL_PROGRAM@|install|' -e 's|@INSTALL_DATA@|install -m 644|' \
	"$S/Makefile.in" > "$S/Makefile"
(cd "$S" && nice -n 19 make -j1 screen) > "$W/build.log" 2>&1 || { tail -20 "$W/build.log"; exit 1; }
! grep 'implicit declaration' "$W/build.log" || exit 1
B=$ROOT/opt/amix
mkdir -p "$B/man/man1" "$B/etc" "$B/share/screen" "$B/doc/screen"
cp "$S/screen" "$B/bin/"
cp "$S/doc/screen.1" "$B/man/man1/"
cp "$S/etc/etcscreenrc" "$B/etc/screenrc"
cp "$S/utf8encodings/"* "$B/share/screen/" 2>/dev/null || :
cp "$S/COPYING" "$S/README" "$B/doc/screen/"
portpkg
