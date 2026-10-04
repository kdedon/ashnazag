#!/bin/sh
set -eu
[ "$#" -eq 2 ] || { echo 'usage: build.sh kernel.elf outdir' >&2; exit 2; }
G=$(cd "$(dirname "$0")/../.." && pwd)
K=$(cd "$G/.." && pwd)
AUX=$(cd "$K/.." && pwd)
. "$AUX/toolchain/src/gcc-cross-amix/build/env.sh"
KERNEL=$1
mkdir -p "$2"
O=$(cd "$2" && pwd)
mkdir -p "$O/tools" "$O/src"
nice -n 19 "${HOSTCC:-cc}" -std=gnu89 -O -w -DDLM_TOOL -I"$K/dlm" \
    -I"$K/dlm/include" -o "$O/tools/mkksym" "$K/dlm/tools/mkksym.c" "$K/dlm/dlm_sym.c"
nice -n 19 "${HOSTCC:-cc}" -std=gnu89 -O -w -o "$O/tools/modfix" "$K/dlm/tools/modfix.c"
sha256sum "$KERNEL" > "$O/kernel.sha256"
"$O/tools/mkksym" -x "$KERNEL" > "$O/exports"
for m in guestcore amigaguest; do
    d=$O/src/$m
    mkdir -p "$d"
    objs=
    for c in "$G/mod/$m"/*.c; do
        obj=$d/$(basename "$c" .c).o
        nice -n 19 m68k-cbm-sysv4-gcc $AMIX_KERNEL_CFLAGS -Wall \
            -I"$K/dlm/include" -I"$G/include" -c "$c" -o "$obj"
        objs="$objs $obj"
    done
    nice -n 19 m68k-cbm-sysv4-ld -r -o "$d/Driver.o" $objs
    cp "$G/mod/$m/Master" "$d/Master"
    DLM_TOOLS=$O/tools nice -n 19 sh "$K/dlm/tools/mkmod" -e "$O/exports" -o "$O" "$d"
done
