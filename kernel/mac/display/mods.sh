#!/bin/sh
# mods.sh kernel.elf outdir [target] -- outdir/mod.d/dsuser for that kernel.
set -e
K=$(cd "$(dirname "$0")/../.." && pwd)
exec sh "$K/mods.sh" TARGET="${3:-$(sh "$K/mods.sh" -t "$1")}" "$2" KERNEL="$1" display
