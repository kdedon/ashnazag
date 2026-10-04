#!/bin/sh
set -eu
G=$(cd "$(dirname "$0")/../.." && pwd)
K=$(cd "$G/.." && pwd)
AUX=$(cd "$K/.." && pwd)
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
O=$(mktemp -d "${TMPDIR:-/tmp}/amigaguest-compile.XXXXXX")
trap 'rm -rf "$O"' EXIT HUP INT TERM
for c in "$G/mod/amigaguest"/*.c "$G/mod/guestcore/guestcore.c"; do
    nice -n 19 m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS -Wall \
        -I"$K/dlm/include" -I"$G/include" -c "$c" -o "$O/$(basename "$c" .c).o"
done
echo 'amigaguest: isolated m68k compilation passed'
