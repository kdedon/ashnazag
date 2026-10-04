#!/bin/sh
set -eu
D=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
A="$D/../../guest/amiga"
O=$(mktemp -d)
trap 'rm -rf "$O"' EXIT HUP INT TERM
nice -n 19 ${CC:-cc} -Wall -Wextra -Werror -I"$A" "$D/sysroot.c" "$A/sysroot.c" -o "$O/sysroot"
"$O/sysroot"
