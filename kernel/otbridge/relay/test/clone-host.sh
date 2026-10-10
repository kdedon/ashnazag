#!/bin/sh
set -eu
D=$(cd "$(dirname "$0")/.." && pwd)
W=${1:?private output directory required}
mkdir -p "$W"
for mode in plain traced; do
    flags=
    if [ "$mode" = traced ]; then flags='-DOTR_DIAGNOSTICS -DOTR_TRACE_TPI'; fi
    nice -n19 ${HOSTCC:-cc} -std=gnu89 -O -Wall -Wextra -Werror -Wno-misleading-indentation \
        -fsanitize=address,undefined $flags -o "$W/clone-$mode" "$D/test/clone.c" \
        "$D/provider.c" "$D/bootstrap.c" "$D/record.c" "$D/message.c"
    ASAN_OPTIONS=detect_leaks=0 nice -n19 "$W/clone-$mode"
done
