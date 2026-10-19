#!/bin/sh
# bootmods.sh kernel.elf outdir -- the disk controller drivers as boot
# modules for that kernel: outdir/mod.d/{amigaide,ncr770}.
exec sh "$(dirname "$0")/../mods.sh" TARGET=amiga "$2" KERNEL="$1" boot
