#!/bin/sh
set -eu
D=$(cd "$(dirname "$0")/.." && pwd)
W=${1:?private output directory required}
mkdir -p "$W"
sh "$D/test/provider-host.sh" "$W/foundation"
nice -n19 ${HOSTCC:-cc} -std=gnu89 -O -Wall -Wextra -Werror -Wno-misleading-indentation \
    -fsanitize=address,undefined -o "$W/mps-test" "$D/mps.c" "$D/provider.c" \
    "$D/bootstrap.c" "$D/record.c" "$D/message.c" "$D/test/mps.c"
ASAN_OPTIONS=detect_leaks=0 nice -n19 "$W/mps-test"
sh "$D/build-provider.sh" "$W/library"
