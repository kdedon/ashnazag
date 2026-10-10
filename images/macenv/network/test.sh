#!/bin/sh
set -eu
D=$(cd "$(dirname "$0")" && pwd)
AUX=$(cd "$D/../../.." && pwd)
W=${1:?private output directory required}
mkdir -p "$W"
nice -n19 ${HOSTCC:-cc} -std=gnu89 -Wall -Wextra -fsanitize=address,undefined \
    -I"$AUX/kernel/dlm/include" -o "$W/test-otreg" "$D/test-otreg.c"
ASAN_OPTIONS=detect_leaks=0 "$W/test-otreg"
