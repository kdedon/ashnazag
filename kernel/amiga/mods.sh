#!/bin/sh
# mods.sh -- the loadable modules for an Amiga (AMIX) kernel.
#
#   sh amiga/mods.sh kernel.elf outdir [target]
#
# Out: outdir/mod.d/{amilib,opci,pci,ne,xsurf,zz9000,voodoo3,tdfx,radeonds,radeon,floppy,v3bars}, built for that kernel's CPU; amilib is
# checked against the kernel's exports (it must carry DLM: dlm_cacheflush),
# opci against those and amilib's.  Install them in /etc/conf/mod.d;
# /etc/conf/pci gets openpci.library (the openpci archive's
# Libs/openpci.library) and, optionally, PCI-Configuration.
set -e
K=$(cd "$(dirname "$0")/.." && pwd)
[ $# -eq 2 ] || [ $# -eq 3 ] || { echo "usage: mods.sh kernel.elf outdir [target]" >&2; exit 2; }
exec sh "$K/mods.sh" TARGET="${3:-$(sh "$K/mods.sh" -t "$1")}" "$2" KERNEL="$1" drivers
