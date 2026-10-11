#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../../.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
nice -n 19 ${CC:-cc} -std=c99 -O -Wall -Wextra -Werror -I"$ROOT/kernel/guest/amiga" \
    "$HERE/amfm.c" "$ROOT/kernel/guest/amiga/amfm.c" -o "$TMP/amfm"
nice -n 19 "$TMP/amfm"
