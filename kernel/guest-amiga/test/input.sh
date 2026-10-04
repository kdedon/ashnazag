#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../../.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
${CC:-cc} -std=c99 -Wall -Wextra -Werror -I"$ROOT/kernel/mac/display" \
    -I"$ROOT/kernel/guest/amiga" "$HERE/input.c" \
    "$ROOT/kernel/guest/amiga/input.c" -o "$TMP/input"
nice -n 19 "$TMP/input"
