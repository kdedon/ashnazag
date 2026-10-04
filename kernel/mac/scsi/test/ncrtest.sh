#!/bin/sh
# ncrtest.sh -- host test of the 53C96 state machine (ncr96.c + ncrsim.c).
#
#   sh kernel/mac/scsi/test/ncrtest.sh WORKDIR [-v] [SCENARIO]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$HERE/../../../.." && pwd)
W="${1:?usage: ncrtest.sh WORKDIR [-v] [SCENARIO]}"
shift
mkdir -p "$W"
nice -n 19 cc -std=gnu89 -w -g -DNCR_HOST -I"$HERE" -I"$HERE/.." \
	-I"$AUX/toolchain/amix-root/usr/sys/amiga/alien" \
	-o "$W/ncrsim" "$HERE/ncrsim.c" "$HERE/../ncr96.c"
nice -n 19 "$W/ncrsim" "$@"
