#!/bin/sh
set -eu
D=$(cd "$(dirname "$0")" && pwd)
W=${1:?private output directory required}
mkdir -p "$W"
nice -n19 ${HOSTCC:-cc} -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    "$D/../admission.c" "$D/admission-host.c" -o "$W/admission-host"
ASAN_OPTIONS=detect_leaks=0 "$W/admission-host"
