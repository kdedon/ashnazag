#!/bin/sh
# mods.sh kernel.elf outdir -- the 53C96 driver as the ncr96 module for
# that kernel: outdir/mod.d/ncr96.
exec sh "$(dirname "$0")/../../mods.sh" TARGET=mac "$2" KERNEL="$1" boot
