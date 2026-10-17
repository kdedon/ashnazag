#!/bin/sh
# mods.sh -- the optional-hardware driver modules for an Atari kernel.
#
#   sh atari/mods.sh kernel.elf outdir [target]
#
# Out: outdir/mod.d/{scsi,sd,aen,dpn,dmasnd,auxsnd}, built for that kernel's
# CPU and checked against its exports.  They go in /etc/conf/mod.d; sysinit
# registers aen (18), dmasnd (46), auxsnd (47) and dpn (58), which load on
# their first open; sd (over scsi) loads on the first open of a card 1 disk.
set -e
K=$(cd "$(dirname "$0")/.." && pwd)
[ $# -eq 2 ] || [ $# -eq 3 ] || { echo "usage: mods.sh kernel.elf outdir [target]" >&2; exit 2; }
exec sh "$K/mods.sh" TARGET="${3:-$(sh "$K/mods.sh" -t "$1")}" "$2" KERNEL="$1" drivers
