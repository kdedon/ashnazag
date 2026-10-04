#!/bin/sh
# getnet.sh -- the network tools the net test root takes from AMIX tape
# segment 07 (net), extracted once into tests/build/net07.
#
#   sh tests/net/getnet.sh [tape-dir]      (default $AMIX_TAPE)
set -e

N=$(cd "$(dirname "$0")" && pwd)
B=$(cd "$N/.." && pwd)/build
TAPE="${1:-$AMIX_TAPE}"
[ -f "$TAPE/07" ] || { echo "[FAIL] no tape segment 07 in '$TAPE'"; exit 1; }
rm -rf "$B/net07"; mkdir -p "$B/net07"
(cd "$B/net07" && cpio -idm --no-absolute-filenames --quiet \
	usr/sbin/ping usr/sbin/route usr/sbin/arp usr/bin/netstat < "$TAPE/07")
ls -l "$B/net07/usr/sbin" "$B/net07/usr/bin"
