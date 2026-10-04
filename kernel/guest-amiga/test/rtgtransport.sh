#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src="$here/../../guest/amiga"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
nice -n 19 "${CC:-cc}" -std=c89 -pedantic -Wall -Wextra -Werror \
    -I"$src" "$here/rtgtransport.c" "$src/rtgtransport.c" -o "$tmp/rtgtransport"
nice -n 19 "$tmp/rtgtransport"
