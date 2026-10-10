#!/bin/sh
set -eu
D=$(cd "$(dirname "$0")/.." && pwd)
W=${1:?private output directory required}
mkdir -p "$W"
nice -n19 ${HOSTCC:-cc} -O1 -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$D/configurator.c" "$D/test/configurator.c" -o "$W/configurator"
ASAN_OPTIONS=detect_leaks=0 "$W/configurator"
