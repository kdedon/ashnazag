#!/bin/sh
# run.sh -- host test of the frame-buffer console.
#
#   sh kernel/mac/video/test/run.sh [outdir]
#
# Builds fbtest with the host compiler (-DFB_HOST: no IPL changes, the
# test's fake memory for low memory and the DAFB) and runs it.  PGM
# images of each rendered mode go to outdir (default: a temp directory,
# removed afterwards unless given).
set -e
T=$(cd "$(dirname "$0")" && pwd)
V=$(cd "$T/.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
OUT="${1:-$W}"
mkdir -p "$OUT"
nice -n 19 cc -std=gnu89 -w -O -DFB_HOST -I"$V" -o "$W/fbtest" \
	"$T/fbtest.c" "$V/fbcons.c" "$V/fbprobe.c" "$V/fbfont.c"
# declaration-ROM probe cases use the Quadra 700/900 ROM image when present
ROM="$(cd "$V/../../.." && pwd)/420DBFF3 - Quadra 700&900 & PB140&170.ROM"
[ -f "$ROM" ] || ROM=""
"$W/fbtest" "$OUT" ${ROM:+"$ROM"}
