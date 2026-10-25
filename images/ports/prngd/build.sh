#!/bin/sh
# build.sh -- cross-build prngd 0.9.29 for /opt/amix.
#
#   sh images/ports/prngd/build.sh outdir
set -e
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
PORT=prngd OUT=$1
. "$AUX/images/ports/port.sh"
R=$ROOT

src prngd > /dev/null
S=$W/prngd-0.9.29
(cd "$S" && patch -p1 -s < "$D/prngd.diff")
for c in prngd bsd-daemon bsd-snprintf close_on_exec conffile error_handlers error_log \
	non_blocking obtain_limit rand seed serverloop sha1 sockio tcp_socket unix_socket; do
	(cd "$S" && $NICE $CC -O -w -D__STDC__=0 -DAMIX -DCONFIGFILE='"/opt/amix/etc/prngd.conf"' \
		-DRANDSAVENAME='"/opt/amix/etc/prngd-seed"' -c $c.c -o $c.o)
done
(cd "$S" && $CC -o prngd *.o -lsocket -lnsl)
mkdir -p "$R/opt/amix/sbin" "$R/opt/amix/etc" "$R/etc/rc2.d"
cp "$S/prngd" "$R/opt/amix/sbin/"
cp "$D/prngd.conf" "$R/opt/amix/etc/"
cp "$D/S72prngd" "$R/etc/rc2.d/"
# its entropy estimates
FPU=prngd
portpkg
