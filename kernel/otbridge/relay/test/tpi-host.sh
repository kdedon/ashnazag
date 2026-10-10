#!/bin/sh
set -eu
D=$(cd "$(dirname "$0")/.." && pwd)
W=${1:?private output directory required}
mkdir -p "$W"
nice -n19 ${HOSTCC:-cc} -DOTR_DIAGNOSTICS -DOTR_TRACE_TPI -std=gnu89 -O \
    -Wall -Wextra -Werror -Wno-misleading-indentation -fsanitize=address,undefined \
    -o "$W/mps" "$D/mps.c" "$D/provider.c" "$D/bootstrap.c" \
    "$D/record.c" "$D/message.c" "$D/test/mps.c"
ASAN_OPTIONS=detect_leaks=0 "$W/mps"
