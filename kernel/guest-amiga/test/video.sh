#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src="$here/../../guest/amiga"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
nice -n 19 "${CC:-cc}" -std=c89 -pedantic -Wall -Wextra -Werror \
    -I"$src" "$here/test_migvideo.c" "$src/migvideo.c" -o "$tmp/test_migvideo"
nice -n 19 "$tmp/test_migvideo"
