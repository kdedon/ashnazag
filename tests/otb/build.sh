#!/bin/sh
# build.sh -- t_otb's module, built against the kernel under test.
#
#   sh tests/otb/build.sh kernel.elf outdir
#
# Out: outdir/mod.d/otbridge.  A kernel tree without otbridge, or a
# kernel without the STREAMS and driver linkages, gets no module (exit
# 0), and t_otb then skips.
set -e

T=$(cd "$(dirname "$0")/.." && pwd)
AUX=$(cd "$T/.." && pwd)
KDIR=${KDIR:-$AUX/kernel}
rm -rf "$2"
if [ ! -f "$KDIR/otbridge/build.sh" ]; then
	echo "[skip] $KDIR has no otbridge"
	exit 0
fi
KDIR=$KDIR sh "$KDIR/otbridge/build.sh" -k "$1" "$2" | grep -v '^PASS'
